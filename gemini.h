#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxGemini {
bool handleCommand(const String& line, Print& out = Serial);
}
