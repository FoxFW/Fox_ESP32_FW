#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxPortal {
bool handleCommand(const String& line, Print& out = Serial);

void loop();
}
