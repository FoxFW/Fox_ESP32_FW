#pragma once

#include <Arduino.h>

#include "fox_reply.h"

// The single top-level AT-command dispatcher, defined in Fox_ESP32_FW.ino:
// tries the handful of bare commands (AT/info/CAPS) and then every
// subsystem's handleCommand()/handleSettingsCommand() in turn, in the same
// fixed order the firmware has always used. This is the exact chain
// Fox_ESP32_FW.ino's own loop() feeds each line read off the physical
// UART - `out` defaults to Serial there, so that call site is unchanged.
//
// FoxLab's HTTP endpoint (task #10, fox_lab.cpp's handleApiCmd()) calls
// this too, with a FoxReplyBuffer as `out`: an esp32-tab HTTP request gets
// routed through literally the same subsystem command handlers a physical
// AT-command would use, with zero duplicated dispatch logic and zero risk
// of the HTTP path and the UART path ever drifting out of sync on which
// subsystems exist or what order they're tried in. The WebSocket streaming
// channel (task #11) is expected to call it the same way for any command
// whose *initial* ack fits a normal reply - streamed follow-up events
// (CSI/mesh, live scan output, etc.) don't go through here at all, since
// those aren't part of any single handleCommand() call's chain - see
// fox_reply.h's "NOT SINK-REDIRECTED" list for exactly which ones.
namespace FoxDispatch {
void handleCommand(const String& line, Print& out = Serial);
}
