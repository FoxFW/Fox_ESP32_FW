#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxScript {
void begin();

// A running script's own print(...) builtin and REBOOTING notice are
// redirected too - see the Interp class's `out` member in
// script_engine.cpp (a plain pointer, not threaded as a parameter,
// because Interp's own methods are too mutually-recursive for that).
bool handleCommand(const String& line, Print& out = Serial);
}
