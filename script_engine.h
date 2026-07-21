#pragma once

#include <Arduino.h>

namespace FoxScript {

void begin();

bool handleCommand(const String& line);

}
