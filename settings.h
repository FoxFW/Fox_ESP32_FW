#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxSettings {
void begin();

bool attacksEnabled();
void setAttacksEnabled(bool enabled);

bool profanityFilterEnabled();
void setProfanityFilterEnabled(bool enabled);

bool expertModeEnabled();
void setExpertModeEnabled(bool enabled);

bool handleSettingsCommand(const String& line, Print& out = Serial);
}
