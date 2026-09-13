#include "fox_remote.h"

// See fox_remote.h for the full picture. This file is deliberately tiny -
// almost everything the old fox_rpc_bridge.cpp needed (a state machine,
// frame encode/decode, a handshake, flow control, baud switching) simply
// doesn't exist in FLPR: it's a plain line relay onto a UART that's always
// already running in AT-command mode.

namespace {
WebSocketsServer* clientServer = nullptr;
int16_t clientNums[FOX_REMOTE_MAX_CLIENTS];
uint8_t clientCount = 0;
}  // namespace

namespace FoxRemote {

void attachClient(WebSocketsServer* server, uint8_t num) {
  clientServer = server;
  if (clientCount >= FOX_REMOTE_MAX_CLIENTS) return;
  for (uint8_t i = 0; i < clientCount; i++) {
    if (clientNums[i] == (int16_t)num) return;  // already attached
  }
  clientNums[clientCount++] = (int16_t)num;
}

void detachClient(uint8_t num) {
  for (uint8_t i = 0; i < clientCount; i++) {
    if (clientNums[i] == (int16_t)num) {
      for (uint8_t j = i; j < clientCount - 1; j++) clientNums[j] = clientNums[j + 1];
      clientCount--;
      break;
    }
  }
  if (clientCount == 0) {
    // Best-effort - if the Flipper isn't listening (FoxLAB app already
    // closed) this just goes nowhere, same as any other reply nobody's
    // there to read. Only fires once the LAST attached client is gone -
    // see fox_remote.h's file-header comment.
    Serial.println("[FLPR/SESSION/END]");
    clientServer = nullptr;
  }
}

void detachAllClients() {
  if (clientCount > 0) {
    Serial.println("[FLPR/SESSION/END]");
  }
  clientCount = 0;
  clientServer = nullptr;
}

bool hasClient() {
  return clientCount > 0;
}

bool isFull() {
  return clientCount >= FOX_REMOTE_MAX_CLIENTS;
}

bool isAttachedClient(uint8_t num) {
  for (uint8_t i = 0; i < clientCount; i++) {
    if (clientNums[i] == (int16_t)num) return true;
  }
  return false;
}

void sendToFlipper(const String& line) {
  Serial.println(line);
}

void forwardToClient(String line) {
  if (!clientServer) return;
  for (uint8_t i = 0; i < clientCount; i++) {
    clientServer->sendTXT((uint8_t)clientNums[i], line);
  }
}

}  // namespace FoxRemote
