#pragma once

#include <Arduino.h>

namespace FoxGps {
bool handleCommand(const String& line);

void loop();

bool getFix(double* latOut, double* lonOut);
}
