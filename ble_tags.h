#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxBleTags {
// NOT SINK-REDIRECTED (partially): the per-device "TAG:..." lines a
// BLETAGSCAN finds (TagScanCallback::onResult, in ble_tags.cpp) stay on
// the physical Serial only - see the comment above that class. The
// TAGSCANDONE:/ERROR summary handleCommand() itself sends is redirected
// to `out` like every other subsystem here.
bool handleCommand(const String& line, Print& out = Serial);
}
