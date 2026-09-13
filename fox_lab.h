#pragma once
#include <Arduino.h>

#include "fox_reply.h"

namespace FoxLab {
void begin();
void loop();
bool handleCommand(const String& line, Print& out = Serial);

// True while FoxLAB's own web UI (WebServer:80 + WebSocketsServer:82/83,
// AP "FoxLAB") is up. Task #13: fox_csi.cpp checks this before starting
// its own web UI (WebServer:80, AP "FoxCSI") for the same reason - see
// FoxCsi::isWebUiActive()'s comment.
bool isActive();
}
