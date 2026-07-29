#pragma once

#include <Arduino.h>

namespace FoxProfanityFilter {
bool containsBlockedContent(const String& text);
}
