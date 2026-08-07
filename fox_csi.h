#pragma once
#include <Arduino.h>

namespace FoxCsi {
void begin();
void loop();
bool handleCommand(const String& line);
}
