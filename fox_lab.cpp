#include "fox_lab.h"
#include "config.h"

#if FOX_HAS_LAB

#include "foxlab_page.h"
#include "fox_remote.h"
#include "fox_dispatch.h"
#include "fox_csi.h"

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>

// FoxLAB: a single on/off switch, flipped from the Flipper's "FoxLAB"
// app, that stands up a WiFi AP ("FoxLAB") + web server serving
// foxlab_page.h's page (foxfw-lab.html, embedded verbatim). Mirrors
// fox_csi.cpp's webUiStart()/webUiStop() pattern. Runs entirely on the
// ESP32 side once started - the Flipper app only needs to be open long
// enough to send the START/STOP command, per FoxLab::handleCommand()
// below having no dependency on an active serial session.
//
// Deliberately not persisted: a reboot always comes back to "off,
// waiting for a command", never auto-resuming whatever was on before
// power was lost (same as fox_csi.cpp's web UI toggle).
//
// The page also gets a Fox Remote ("FLPR") relay WebSocket (see
// fox_remote.h) on its own port, separate from fox_csi.cpp's WS port 81 -
// see FOX_LAB_REMOTE_WS_PORT below. Unlike LAB/START and LAB/STOP, which
// are AT-commands the *Flipper app* sends over the shared UART, the relay
// is started/stopped by the *browser* opening/closing that WebSocket - the
// Flipper has no reason to ask the ESP32 to start a relay session with
// itself. Unlike the old Expansion-Protocol-based bridge this replaced,
// the relay never touches the shared UART's baud rate or framing - it's
// just more lines of the same AT-command protocol [LAB/...] already uses,
// so Fox_ESP32_FW.ino's own Serial line reader never has to step aside for
// it (see FoxRemote::forwardToClient(), called from that same reader for
// any "[FLPR/" line instead of going through FoxDispatch::handleCommand()).
//
// Task #10 adds a second thing to this same WebServer: POST /api/cmd,
// the esp32-tab's request/response command endpoint (see handleApiCmd()
// below) - this is how the page's "esp32" tab controls WiFi/BLE/GPS/IR/
// RFID/SubGHz/Settings/AI etc. over WiFi instead of the USB Web-Serial
// connection it used before. Task #11 adds a third: labCmdWsServer, a
// WebSocketsServer on its own port (83 - 80/81 are fox_csi.cpp's, 82 is
// labRemoteWsServer above) for the same command vocabulary, but delivering
// each reply line live as its own WS frame instead of one buffered HTTP
// response - the right fit for commands whose output is unbounded/
// ongoing (live sniff/wardrive monitor lines, multi-second scans). See
// onLabCmdWsEvent()/FoxWsPrint below.

namespace {

#define FOX_LAB_AP_SSID "FoxLAB"
#define FOX_LAB_AP_PASS "88888888"
#define FOX_LAB_AP_MAX_CONN 4
#define FOX_LAB_REMOTE_WS_PORT 82
#define FOX_LAB_CMD_WS_PORT 83
#define FOX_LAB_WS_LINE_MAX 512
#define FOX_LAB_AP_IP IPAddress(192, 168, 4, 1)
#define FOX_LAB_HOSTNAME "foxlab.local"

WebServer* labHttpServer = nullptr;
bool labActive = false;

WebSocketsServer* labRemoteWsServer = nullptr;

WebSocketsServer* labCmdWsServer = nullptr;

// Gives FoxLAB's AP a friendly `foxlab.local` name alongside the raw IP,
// without relying on mDNS (ESPmDNS) - see the project's FOXLAB_HOSTNAME_
// RESEARCH.md for why: mDNS/multicast on this ESP32's AP interface (this
// app runs WIFI_AP_STA, not plain WIFI_AP - see labStart() below) has
// several long-standing, still-open upstream reliability issues specific
// to phones/tablets connecting with no internet gateway, which is exactly
// FoxLAB's own primary audience. Plain unicast DNS on port 53 doesn't have
// that problem - every OS already queries whatever DNS server its DHCP
// lease points it at, no client-side mDNS daemon required - and this repo
// already has a proven, working example of exactly this: fox_portal.cpp's
// own `dnsServer`. The one difference from that: fox_portal.cpp wildcards
// every domain ("*") on purpose, to force a captive-portal redirect: this
// answers only "foxlab.local" specifically (DNSServer's other start()
// overload) and leaves every other lookup alone, since there's no redirect
// goal here, just a friendly name alongside the IP address, which stays
// the documented, reliable primary (see start()'s call site below for the
// one real caveat: a phone with encrypted "Private DNS" enabled bypasses
// this entirely).
DNSServer labDnsServer;

// Task #11: a Print sink that turns each line a command handler writes
// into its own WebSocket text frame, sent as soon as that line is
// complete - not buffered up and sent all at once the way FoxReplyBuffer
// (fox_reply.h, task #9/#10) is. WebSocketsServer::sendTXT() writes
// straight to the client's TCP socket and doesn't need labCmdWsServer's
// own loop() to be pumping in order to send, so this works even from deep
// inside a long-running handler that's still blocking (e.g. wifi_recon.cpp's
// runWardrive() looping with delay()s between prints) - each line reaches
// the browser the moment it's printed, which is the entire point of this
// channel over the HTTP endpoint's buffer-then-send-once behavior.
class FoxWsPrint : public Print {
 public:
  FoxWsPrint(WebSocketsServer* server, uint8_t clientNum)
      : _server(server), _clientNum(clientNum) {}

  size_t write(uint8_t c) override {
    if (c == '\n') {
      flushLine();
    } else if (c != '\r') {
      if ((int)_line.length() < FOX_LAB_WS_LINE_MAX) _line += (char)c;
    }
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }

  // Some handlers' very last Serial.print() of a reply isn't followed by
  // a println() - call this once the command has returned so a trailing
  // partial line still reaches the browser instead of being dropped.
  void finish() {
    if (_line.length() > 0) flushLine();
  }

 private:
  void flushLine() {
    if (_server) _server->sendTXT(_clientNum, _line);
    _line = "";
  }

  WebSocketsServer* _server;
  uint8_t _clientNum;
  String _line;
};

void handleLabRoot() {
  labHttpServer->send_P(200, "text/html", FOXLAB_HTML);
}

// Task #14 (Paint tab): serves foxlab_src/wallpaper_painter.html, a
// FoxLAB-specific fork of FOX_WEB's Wallpaper Painter vendored into
// foxlab_page.h alongside FOXLAB_HTML - see that page's own file-top
// comment for what's different (no protobufjs/Web-Serial; talks to the
// Flipper through this page's own FLPR relay instead) and
// gen_foxlab_page.js's comment for why it's vendored rather than shared
// with FOX_WEB at build time. Registered as its own route instead of
// falling through onNotFound()'s FOXLAB_HTML catch-all, which is what the
// Lab page's Paint tab iframe (data-src="wallpaper-painter.html") and its
// "open in its own tab" link were both silently hitting before this
// existed - real-hardware feedback, 2026-09-11.
void handlePainterPage() {
  labHttpServer->send_P(200, "text/html", FOXLAB_PAINTER_HTML);
}

// Task #10: the esp32-tab's request/response command endpoint. POST body
// is a single AT-command line - exactly the same text the Flipper's Fox
// app would send over the shared UART (see fox_dispatch.h) - and the
// response body is whatever that command's handler would have printed to
// Serial, captured via a FoxReplyBuffer instead. This is a WebServer route
// handler (async, invoked by labHttpServer->handleClient() in loop() below,
// same NOT-SINK-REDIRECTED category as handleLabRoot() above - it isn't
// itself part of any handleCommand() call's chain), but it's exactly the
// convergence point the reply-sink abstraction (task #9) was built for: it
// opens its own sink and feeds it into the SAME dispatch chain the UART
// reader uses, so every subsystem's command surface is reachable here for
// free, with no per-command endpoint code and no way for the HTTP surface
// to drift from what AT-commands actually support.
//
// Deliberately not a fit for every command: something whose real reply is
// an unbounded/ongoing stream (live sniff/wardrive monitor output, CSI/
// mesh events, etc. - see fox_reply.h's NOT-SINK-REDIRECTED list) will
// still "work" here in the sense that FoxReplyBuffer captures whatever it
// prints before returning, but the HTTP response can't complete until the
// command does, so the browser sees nothing until the whole thing is over
// and holds the connection open meanwhile. Task #11's WebSocket channel is
// for those; task #12 (the page rewrite) is what decides, per command,
// which transport to use.
void handleApiCmd() {
  String line = labHttpServer->arg("plain");
  line.trim();

  labHttpServer->sendHeader("Connection", "close");

  if (line.length() == 0) {
    labHttpServer->send(400, "text/plain", "ERROR:EMPTY");
    return;
  }

  FoxReplyBuffer buf;
  FoxDispatch::handleCommand(line, buf);
  labHttpServer->send(200, "text/plain", buf.str());
}

// The Fox Remote relay WebSocket's event handler. A browser client
// connects, sends "[FLPR/...]" command lines as WS TEXT frames, and gets
// "[FLPR/...]" reply/push lines back the same way - see fox_remote.h for
// which direction does what. Every client that connects is attached (and
// gets every reply/push line broadcast to it) up to FoxRemote::isFull()'s
// cap; a connection arriving once that cap is reached is refused outright.
void onLabRemoteWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      if (FoxRemote::isFull()) {
        labRemoteWsServer->disconnect(num);
        return;
      }
      FoxRemote::attachClient(labRemoteWsServer, num);
      break;

    case WStype_DISCONNECTED:
      // Only tear down if this was actually an attached client - a
      // disconnect event for some OTHER client we just forcibly rejected
      // above (the "cap reached" branch) would otherwise incorrectly detach
      // a real one. See FoxRemote::isAttachedClient()'s comment.
      if (FoxRemote::isAttachedClient(num)) {
        FoxRemote::detachClient(num);
      }
      break;

    case WStype_TEXT: {
      String line;
      line.reserve(length);
      for (size_t i = 0; i < length; i++) line += (char)payload[i];
      line.trim();
      if (line.length() > 0) FoxRemote::sendToFlipper(line);
      break;
    }

    default:
      break;
  }
}

// Task #11: the esp32-tab's streaming command channel. A client sends one
// AT-command line as a WS text frame, same vocabulary as POST /api/cmd
// (handleApiCmd() above) and the physical UART - the difference is purely
// in how the reply is delivered: each line the command handler prints goes
// out as its own WS text frame via a fresh FoxWsPrint, live, rather than
// being buffered into one HTTP response. That's what makes this channel
// the right fit for commands with unbounded/ongoing output (live sniff/
// wardrive monitor lines, multi-second scans, etc.) where the HTTP
// endpoint would otherwise hold the connection open in silence until the
// whole thing finished. Every command still goes through the exact same
// FoxDispatch::handleCommand() chain either way - no duplicated dispatch
// logic, no way for this channel's command surface to drift from the
// UART's or the HTTP endpoint's.
//
// One sink per incoming line (not one shared per-connection sink): a
// client firing a second command before the first one's handler returns
// isn't actually possible here, since this whole event callback runs
// synchronously inside labCmdWsServer->loop() - there's no way for a
// second WStype_TEXT event for the same client to be processed until this
// one returns - but scoping the sink to a single dispatch call keeps that
// invariant obviously true from the code alone rather than relying on it.
void onLabCmdWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_TEXT: {
      String line;
      line.reserve(length);
      for (size_t i = 0; i < length; i++) line += (char)payload[i];
      line.trim();
      if (line.length() == 0) return;

      FoxWsPrint sink(labCmdWsServer, num);
      FoxDispatch::handleCommand(line, sink);
      sink.finish();
      break;
    }

    default:
      break;
  }
}

void labStart() {
  if (labActive) return;

  uint8_t apChannel = WiFi.channel();
  if (apChannel < 1 || apChannel > 13) apChannel = 6;
  WiFi.mode(WIFI_AP_STA);

  // Explicit AP IP/gateway/DNS config (matching fox_portal.cpp's own
  // softAPConfig() call) rather than relying on the core's undocumented
  // default DHCP behavior - the fifth parameter is what actually gets
  // handed to clients as their DNS server, which labDnsServer.start()
  // below depends on. FOX_LAB_AP_IP is the same 192.168.4.1 this AP has
  // always used (every existing doc/screen already names that address),
  // this doesn't change it - it just makes the DNS piece explicit instead
  // of assumed.
  WiFi.softAPConfig(FOX_LAB_AP_IP, FOX_LAB_AP_IP, IPAddress(255, 255, 255, 0), IPAddress(0, 0, 0, 0), FOX_LAB_AP_IP);

  // ESP32's softAP() rejects any passphrase shorter than 8 characters
  // outright (WPA2-PSK's minimum) - FOX_LAB_AP_PASS is exactly 8, so
  // this always takes the real-password branch. Kept as a guard in
  // case that constant is ever shortened again (same fallback
  // http_bridge.cpp already uses for its own AP).
  if (strlen(FOX_LAB_AP_PASS) >= 8) {
    WiFi.softAP(FOX_LAB_AP_SSID, FOX_LAB_AP_PASS, apChannel, 0, FOX_LAB_AP_MAX_CONN);
  } else {
    WiFi.softAP(FOX_LAB_AP_SSID, nullptr, apChannel, 0, FOX_LAB_AP_MAX_CONN);
  }

  // Domain-scoped, not wildcard (see labDnsServer's own comment above) -
  // answers only "foxlab.local" with this AP's IP, leaves every other
  // lookup alone.
  labDnsServer.start(53, FOX_LAB_HOSTNAME, FOX_LAB_AP_IP);

  if (!labHttpServer) labHttpServer = new WebServer(80);
  labHttpServer->on("/", HTTP_GET, handleLabRoot);
  labHttpServer->on("/wallpaper-painter.html", HTTP_GET, handlePainterPage);
  labHttpServer->on("/api/cmd", HTTP_POST, handleApiCmd);
  labHttpServer->onNotFound(handleLabRoot);
  labHttpServer->begin();

  if (!labRemoteWsServer) labRemoteWsServer = new WebSocketsServer(FOX_LAB_REMOTE_WS_PORT);
  labRemoteWsServer->begin();
  labRemoteWsServer->onEvent(onLabRemoteWsEvent);

  if (!labCmdWsServer) labCmdWsServer = new WebSocketsServer(FOX_LAB_CMD_WS_PORT);
  labCmdWsServer->begin();
  labCmdWsServer->onEvent(onLabCmdWsEvent);

  labActive = true;
}

void labStop() {
  if (!labActive) return;
  if (labHttpServer) labHttpServer->stop();
  if (labRemoteWsServer) labRemoteWsServer->close();
  if (labCmdWsServer) labCmdWsServer->close();
  labDnsServer.stop();
  // Sends [FLPR/SESSION/END] to the Flipper first if any client was
  // actually attached - see FoxRemote::detachAllClients()'s own comment.
  FoxRemote::detachAllClients();
  WiFi.softAPdisconnect(true);
  if (WiFi.getMode() == WIFI_AP_STA) WiFi.mode(WIFI_STA);
  labActive = false;
}

}  // namespace

namespace FoxLab {

void begin() {
  // No auto-resume on boot by design - see the file-header comment.
}

void loop() {
  if (labActive && labHttpServer) labHttpServer->handleClient();
  if (labActive && labRemoteWsServer) labRemoteWsServer->loop();
  if (labActive && labCmdWsServer) labCmdWsServer->loop();
  if (labActive) labDnsServer.processNextRequest();
}

bool isActive() {
  return labActive;
}

bool handleCommand(const String& line, Print& out) {
#pragma push_macro("Serial")
#undef Serial
#define Serial out
  if (!line.startsWith("[LAB/")) return false;
  int closeBracket = line.indexOf(']');
  if (closeBracket < 0) return false;
  String cmd = line.substring(1, closeBracket);

  if (cmd == "LAB/STATUS") {
    Serial.print("[LAB/STATUS/SUCCESS]{\"active\":");
    Serial.print(labActive ? 1 : 0);
    Serial.println("}");
    return true;
  }

  if (cmd == "LAB/START") {
    // Task #13: the ESP32 can only run one softAP SSID and bind port 80
    // once - starting FoxLAB while FoxCSI's own web UI (AP "FoxCSI") is up
    // would silently rename the AP out from under any already-connected
    // FoxCSI browser client and likely fail FoxLAB's own port-80 bind too.
    // Refuse cleanly instead of leaving both in a broken half-started state.
    if (FoxCsi::isWebUiActive()) {
      Serial.println("[LAB/START/ERROR]CSIACTIVE");
      return true;
    }
    labStart();
    Serial.println("[LAB/START/SUCCESS]");
    return true;
  }

  if (cmd == "LAB/STOP") {
    labStop();
    Serial.println("[LAB/STOP/SUCCESS]");
    return true;
  }

  return false;
#undef Serial
#pragma pop_macro("Serial")
}

}  // namespace FoxLab

#else

// Not enough free RAM on this board (see config.h's FOX_HAS_LAB) - stub
// out the whole feature. Any [LAB/...] command gets a clear "not
// supported here" reply instead of silently timing out.
namespace FoxLab {

void begin() {}
void loop() {}

bool isActive() {
  return false;
}

bool handleCommand(const String& line, Print& out) {
#pragma push_macro("Serial")
#undef Serial
#define Serial out
  if (!line.startsWith("[LAB/")) return false;
  int closeBracket = line.indexOf(']');
  if (closeBracket < 0) return false;
  String cmd = line.substring(1, closeBracket);

  if (cmd == "LAB/STATUS" || cmd == "LAB/START" || cmd == "LAB/STOP") {
    Serial.print('[');
    Serial.print(cmd);
    Serial.println("/ERROR]NOLAB");
    return true;
  }

  return false;
#undef Serial
#pragma pop_macro("Serial")
}

}  // namespace FoxLab

#endif
