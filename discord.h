#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxDiscord {
bool handleCommand(const String& line, Print& out = Serial);
}
