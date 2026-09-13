#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxBleAttack {
bool handleCommand(const String& line, Print& out = Serial);

bool scriptSpam(const String& mode);
}
