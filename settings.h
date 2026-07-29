#pragma once

#include <Arduino.h>

namespace FoxSettings {
void begin();

bool attacksEnabled();
void setAttacksEnabled(bool enabled);

bool profanityFilterEnabled();
void setProfanityFilterEnabled(bool enabled);

bool handleSettingsCommand(const String& line);
}
