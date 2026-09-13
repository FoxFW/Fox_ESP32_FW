#pragma once

#include <Arduino.h>
#include <WebSocketsServer.h>

// The ESP32-side relay for the Fox Remote ("FLPR") protocol - replaces
// fox_rpc_bridge.h's role entirely. See the project's Fox Remote Protocol
// design doc for the full rationale and wire format; short version: the
// browser and the Flipper's "FoxLAB" app now talk plain "[FLPR/...]" text
// lines over the same physical UART "[LAB/...]" already uses reliably, one
// WebSocket TEXT frame per line, instead of a binary Expansion Protocol
// tunnel with its own handshake/baud-switch/keep-alive machinery. That
// machinery - and the exclusive "step aside" mode Fox_ESP32_FW.ino's own
// Serial line reader used to need while it ran - is gone: this relay never
// touches the UART's baud rate or framing, so the line reader never has to
// stop running.
//
// Direction browser->Flipper: fox_lab.cpp's relay WebSocket (port 82, same
// port number the old binary bridge used) hands each TEXT frame it
// receives straight to sendToFlipper() below, which writes it to Serial as
// one line.
//
// Direction Flipper->browser: Fox_ESP32_FW.ino's own Serial line reader
// calls forwardToClient() for any complete line that starts with "[FLPR/"
// (instead of routing it through FoxDispatch::handleCommand() like every
// other line) - this relays it verbatim to every browser client currently
// attached, as one WS TEXT frame each.
//
// fox_lab.cpp still owns the actual WebSocketsServer instance and its
// begin()/close()/loop() lifecycle and event callback (labRemoteWsServer,
// onLabRemoteWsEvent()) - attachClient()/detachClient() below just give
// this module (and Fox_ESP32_FW.ino, which has no other reason to know
// about WebSockets at all) a way to reach whichever clients are currently
// attached.
//
// Multiple clients, on purpose: the FoxLAB AP is a shared, unauthenticated
// hotspot (see fox_lab.cpp's FOX_LAB_AP_MAX_CONN) - anyone connected to it
// can already reach every other feature on the page simultaneously, so FLPR
// singling itself out as "only one browser at a time" was an arbitrary,
// user-visible inconsistency (a second tab, or a second person's phone,
// could open the page and use WiFi/BLE/GPS/etc. fine but silently couldn't
// reach the Flipper at all). This module now attaches every WebSocket
// client that connects, up to FOX_REMOTE_MAX_CLIENTS, and broadcasts every
// Flipper->browser line to all of them. There is deliberately no per-client
// request/response correlation - the FLPR wire protocol has no request IDs
// to do that with, and adding one would mean changing foxr_companion.c's
// reply format too - so if two clients both send commands close together
// they'll each see the other's replies mixed into their own log, same as
// two people typing into the same shared terminal. Real-hardware feedback,
// 2026-09-11: this is the explicitly preferred behavior over an "already in
// use" rejection - a shared session multiple people can step on is more
// useful here than a lock only one of them can hold.
namespace FoxRemote {

#define FOX_REMOTE_MAX_CLIENTS 4

// Called from fox_lab.cpp's onLabRemoteWsEvent() when a browser client
// connects to the relay WebSocket. fox_lab.cpp is responsible for
// rejecting a connection once FOX_REMOTE_MAX_CLIENTS is already attached
// (see isFull()) before calling this - a no-op here if called past that
// cap. Re-attaching a `num` that's already attached (shouldn't happen) is
// also a no-op rather than a duplicate entry.
void attachClient(WebSocketsServer* server, uint8_t clientNum);

// Called from fox_lab.cpp whenever one attached client goes away - a real
// WStype_DISCONNECTED event for that client. If this was the LAST attached
// client, this also sends "[FLPR/SESSION/END]" to the Flipper, so the
// FoxLAB Companion releases any held input keys, stops an active screen
// stream, and closes any open write file - the same cleanup a graceful
// SESSION/END from the browser would trigger, but guaranteed to happen even
// if every browser tab just closed without saying goodbye. While at least
// one other client is still attached, no SESSION/END is sent - the session
// stays alive for them. Safe to call with `num` not currently attached (a
// no-op) - callers don't need to guard on isAttachedClient() themselves.
void detachClient(uint8_t clientNum);

// Called from fox_lab.cpp's labStop() - FoxLAB itself shutting down, not
// any one client leaving. Detaches every attached client at once and sends
// "[FLPR/SESSION/END]" if any were attached, same as detachClient() would
// for the last one.
void detachAllClients();

bool hasClient();

// True once FOX_REMOTE_MAX_CLIENTS clients are attached - fox_lab.cpp uses
// this in onLabRemoteWsEvent()'s WStype_CONNECTED case to refuse any
// further incoming connection once the cap is reached (matches
// FOX_LAB_AP_MAX_CONN, the AP's own simultaneous-station limit, so in
// practice every station that can join the AP at all can also get an FLPR
// slot).
bool isFull();

// True if `num` is one of the currently attached clients - fox_lab.cpp
// needs this to tell a real disconnect of an attached client apart from a
// WStype_DISCONNECTED event for some OTHER client it just forcibly
// rejected (see onLabRemoteWsEvent()'s WStype_CONNECTED case, which
// disconnects any connection arriving once isFull() is already true):
// without this check, that rejected client's own disconnect event could
// otherwise be mistaken for one of the real, attached clients leaving.
bool isAttachedClient(uint8_t num);

// Sends one FLPR line to the Flipper over the shared UART - called from
// fox_lab.cpp's WStype_TEXT case with whatever a browser client just sent.
// Any attached client can send; there's no per-client ownership of the
// underlying UART session (see the file header comment above).
void sendToFlipper(const String& line);

// Called from Fox_ESP32_FW.ino's Serial line reader for any complete line
// starting with "[FLPR/" - relays it verbatim to every attached browser
// client as its own WS TEXT frame. Silently drops the line if no client is
// attached (same "nobody's listening" behavior as an unheard [LAB/...]
// reply - nothing to recover here). Takes `line` by value (not const&) -
// WebSocketsServer::sendTXT() wants a mutable String&, same as
// FoxWsPrint::flushLine() already passes it elsewhere in fox_lab.cpp; a
// local copy here is the simplest way to satisfy that without depending on
// a const-ref overload existing, and reusing the same local copy for each
// attached client's sendTXT() call avoids re-copying `line` per recipient.
void forwardToClient(String line);

}  // namespace FoxRemote
