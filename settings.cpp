#include "settings.h"
#include <Preferences.h>

namespace {
  Preferences prefs;
  bool cachedAttacksEnabled = false;
  bool cachedProfanityFilterEnabled = true;
  bool started = false;
}

namespace FoxSettings {
void begin() {
  prefs.begin("foxsettings", false);

  cachedAttacksEnabled = prefs.getBool("attacks", false);
  cachedProfanityFilterEnabled = prefs.getBool("profanity", true);
  started = true;
}

bool attacksEnabled() {
  return started && cachedAttacksEnabled;
}

void setAttacksEnabled(bool enabled) {
  cachedAttacksEnabled = enabled;
  if (started) {
    prefs.putBool("attacks", enabled);
  }
}

bool profanityFilterEnabled() {
  return !started || cachedProfanityFilterEnabled;
}

void setProfanityFilterEnabled(bool enabled) {
  cachedProfanityFilterEnabled = enabled;
  if (started) {
    prefs.putBool("profanity", enabled);
  }
}

bool handleSettingsCommand(const String& line) {
  if (line == "SETTINGS") {
    Serial.print("ATTACKS:");
    Serial.println(attacksEnabled() ? "ON" : "OFF");
    Serial.print("PROFANITY:");
    Serial.println(profanityFilterEnabled() ? "ON" : "OFF");
    return true;
  }

  if (line.startsWith("SETTINGS:")) {
    String rest = line.substring(9);
    int sep = rest.indexOf(':');
    if (sep < 0) {
      Serial.println("ERROR");
      return true;
    }
    String key = rest.substring(0, sep);
    String value = rest.substring(sep + 1);
    key.trim();
    value.trim();
    key.toUpperCase();
    value.toUpperCase();

    if (key == "ATTACKS") {
      if (value == "ON") {
        setAttacksEnabled(true);
        Serial.println("OK");
      } else if (value == "OFF") {
        setAttacksEnabled(false);
        Serial.println("OK");
      } else {
        Serial.println("ERROR");
      }
      return true;
    }

    if (key == "PROFANITY") {
      if (value == "ON") {
        setProfanityFilterEnabled(true);
        Serial.println("OK");
      } else if (value == "OFF") {
        setProfanityFilterEnabled(false);
        Serial.println("OK");
      } else {
        Serial.println("ERROR");
      }
      return true;
    }

    Serial.println("ERROR");
    return true;
  }

  return false;
}
}
