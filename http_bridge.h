#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxHttp {
void begin();

void loop();

// `out` defaults to Serial so the existing UART AT-command call site
// (Fox_ESP32_FW.ino) is unchanged. FoxLAB's HTTP endpoints (task #10) pass
// a FoxReplyBuffer instead. See fox_reply.h for the full design note - a
// few commands (DOWNLOAD/*, BAUD/SET, SOCKET/*) are UART-only and are not
// affected by `out` at all; look for "NOT SINK-REDIRECTED" in
// http_bridge.cpp.
bool handleCommand(const String& line, Print& out = Serial);
}
