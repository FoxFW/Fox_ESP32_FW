#pragma once

// Reply-sink abstraction for Fox subsystem command handlers.
//
// Every subsystem's `handleCommand(const String& line)` used to reply by
// calling `Serial.print()/println()/printf()` directly, because the only
// caller was Fox_ESP32_FW.ino's UART line reader (AT-commands arriving
// from the Flipper's Fox app over the shared UART). FoxLAB's WiFi-based
// ESP32 tab (tasks #10/#11) needs those exact same command handlers to
// answer an HTTP request or a WebSocket client instead - without
// duplicating every subsystem's command logic.
//
// The fix: every `handleCommand()` (and any inner helper that builds part
// of a reply) now takes an extra `Print& out` parameter. On the public
// `handleCommand()` entry point it's defaulted to `Serial`, so every
// existing call site (Fox_ESP32_FW.ino's UART reader) is unchanged and
// keeps behaving exactly as before. Internally, each handler's body still
// reads as if it were writing to `Serial` - `#define Serial out` right
// after the opening brace and `#undef Serial` right before the closing
// brace transparently redirects every `Serial.print/println/printf/write`
// call in that function to whichever sink the caller passed in, with zero
// risk of missing one and zero change to the reply text/formatting itself.
// Pattern:
//
//   bool FoxWifiRecon::handleCommand(const String& line, Print& out) {
//   #pragma push_macro("Serial")
//   #undef Serial
//   #define Serial out
//     ... existing body, unchanged ...
//   #undef Serial
//   #pragma pop_macro("Serial")
//   }
//
// (the `= Serial` default lives only on the declaration in the header,
// never repeated on the .cpp definition - that's a normal C++ default-
// argument rule, not specific to this pattern.)
//
// **push_macro/pop_macro, not a bare #undef Serial at the end** - found the
// hard way on real ESP32-S2 hardware (Arduino IDE, Sketch > Export): on at
// least this target/core version, the real Arduino-ESP32 core's `Serial`
// is itself a *macro* (something like `#define Serial Serial0`), not a
// plain extern object. A bare `#undef Serial` after this pattern's own
// `#define Serial out` doesn't restore that original macro - `#undef`
// can't "pop" a prior definition, it just deletes whatever definition
// currently exists, so the *original* `Serial` macro that `#define Serial
// out` overwrote is gone for the rest of that translation unit once
// undef'd. Any function *later in the same .cpp file* that references
// bare `Serial` again - another `Print& out = Serial` default argument, or
// a NOT-SINK-REDIRECTED function's body that's still hardcoded to
// `Serial` - then fails to compile ("'Serial' was not declared in this
// scope"), while everything *before* that point in the file was and
// remains fine. This is exactly why it only ever showed up in files with
// more than one `#define Serial out`/`#undef Serial` pair (or a NOT-SINK-
// REDIRECTED `Serial` reference after the last pair) - single-pair files
// happened to get lucky. `#pragma push_macro("Serial")` /
// `#pragma pop_macro("Serial")` (both real GCC/Clang extensions, safe on
// the ESP32 Arduino toolchain) save and restore whatever `Serial` actually
// was beforehand - macro or plain object, doesn't matter - so this is
// correct on every board/core-version combination, not just the ones
// where `Serial` happens to be a plain extern object. The synchk g++
// stub-header harness (see the plan doc) never caught this because its
// hand-stubbed `Arduino.h` declares `Serial` as a real object, never a
// macro - a real gap in that harness worth remembering, not just a fixed
// bug.
//
// Two concrete non-UART sinks:
//
//   FoxReplyBuffer  - accumulates the whole reply into a String, for a
//                      synchronous HTTP request/response endpoint (task
//                      #10, fox_lab.cpp's POST /api/cmd - handleApiCmd())
//                      that needs the complete body before it can call
//                      server.send(...).
//   FoxWsPrint      - task #11's streaming sink, one WS text frame per
//                      reply line instead of one buffered response. Lives
//                      in fox_lab.cpp itself (onLabCmdWsEvent(), port 83)
//                      rather than here, to avoid pulling
//                      <WebSocketsServer.h> into every subsystem file that
//                      includes this header for the Print& type alone.
//
// NOT every handleCommand() sub-command is redirected through `out`. A
// command that is genuinely UART/hardware-specific - raw binary framing,
// live Serial.available() polling mid-reply, baud-rate switching, or an
// async event-driven push that isn't part of any single request's call
// chain (a WebSocketsClient/WebSocketsServer event callback, a WebServer
// route handler, a WiFi/BLE driver callback, a periodic push from a
// subsystem's own loop()) - is left hardcoded to the physical `Serial` and
// is NOT reachable from the WiFi dispatch tables tasks #10/#11 build.
// Search each subsystem file for the comment "NOT SINK-REDIRECTED" to find
// each one and why. Full list as of task #9 (every subsystem .h/.cpp pair
// has the reply-sink treatment applied - this is the complete set of
// deliberate exclusions, not a partial scan):
//
//   http_bridge.cpp   - DOWNLOAD/START, DOWNLOAD/STREAM, DOWNLOAD/CANCEL,
//                        BAUD/SET, SOCKET/START, SOCKET/STOP, SOCKET/SEND,
//                        and its wsEventHandler() callback.
//   wifi_recon.cpp    - promiscuousCallback() (IRAM_ATTR WiFi driver RX
//                        callback).
//   ble_bridge.cpp    - ScanCallback (BLE scan found-device lines) and
//                        notifyCallback (async BLE notification callback).
//   ble_tags.cpp      - TagScanCallback.
//   fox_lab.cpp       - handleLabRoot() (WebServer route handler) and
//                        onLabRemoteWsEvent()/the Fox Remote ("FLPR") relay
//                        path (see fox_remote.h) - a browser command line
//                        is forwarded straight to the Flipper over Serial,
//                        and a Flipper reply/push line is forwarded straight
//                        back to the browser (Fox_ESP32_FW.ino's own line
//                        reader, not this file's dispatch chain, is what
//                        routes "[FLPR/" lines there) - neither direction
//                        ever goes through FoxDispatch::handleCommand() at
//                        all, so there's no reply-sink call to redirect.
//   fox_portal.cpp    - handleServe()/handleRedirect()/handleSubmit()
//                        (WebServer route handlers) and logSubmission()
//                        (only ever called from handleSubmit()), plus
//                        FoxPortal::loop()'s own sentinel-probe debug line.
//   fox_csi.cpp       - wifiCsiCb()/surveyPromiscCb() (IRAM_ATTR WiFi
//                        driver callbacks) and FoxCsi::loop()'s periodic
//                        event reports (MOTION/WATERFALL/VITALS/
//                        CHANNEL/SET/NODE_FOUND/MESH) - these are pushed
//                        on the main loop tick, not replies within a
//                        handleCommand() call, and are already dual-
//                        published to the existing CSI web UI's WebSocket
//                        (port 81) via wsBroadcast(). CSI/CHANNEL/AUTO in
//                        particular: handleCommand() only kicks off
//                        runChannelSurvey() and sets a pending flag: the
//                        actual "[CSI/CHANNEL/SET]" reply is printed later
//                        from loop(), not synchronously from the command.
//                        The esp32-tab's streaming channel (task #11)
//                        should tap these *Pending flags/events directly,
//                        the same way wsBroadcast() already does, rather
//                        than trying to route them through `out`.

#include <Arduino.h>
#include <Print.h>

// Print-compatible sink that accumulates into a String. Used by HTTP
// handlers (task #10) that need the full reply body before they can send
// a single HTTP response.
class FoxReplyBuffer : public Print {
 public:
  size_t write(uint8_t c) override {
    _buf += (char)c;
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    _buf.reserve(_buf.length() + size);
    for (size_t i = 0; i < size; i++) _buf += (char)buffer[i];
    return size;
  }
  const String& str() const { return _buf; }
  void clear() { _buf = ""; }

 private:
  String _buf;
};
