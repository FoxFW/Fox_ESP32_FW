#pragma once

#include <Arduino.h>

namespace FoxBleAttack {

bool handleCommand(const String& line);

bool scriptSpam(const String& mode);

}
