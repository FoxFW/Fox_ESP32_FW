#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxIr {
bool handleCommand(const String& line, Print& out = Serial);
}
