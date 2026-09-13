#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxGps {
bool handleCommand(const String& line, Print& out = Serial);

void loop();

bool getFix(double* latOut, double* lonOut);
}
