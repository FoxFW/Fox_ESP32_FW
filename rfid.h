#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxRfid {
bool handleCommand(const String& line, Print& out = Serial);
}
