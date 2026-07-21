#pragma once

#include <Arduino.h>

namespace FoxHttp {

void begin();

void loop();

bool handleCommand(const String& line);

}
