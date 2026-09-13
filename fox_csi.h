#pragma once
#include <Arduino.h>

#include "fox_reply.h"

namespace FoxCsi {
void begin();
void loop();
bool handleCommand(const String& line, Print& out = Serial);

// True while CSI's own web UI (WebServer:80 + WebSocketsServer:81, AP
// "FoxCSI") is up. Task #13: FoxLab checks this before starting its own
// web UI (WebServer:80, AP "FoxLAB") - the ESP32 can only run one softAP
// SSID and bind port 80 once, so the two must never be active together.
bool isWebUiActive();
}
