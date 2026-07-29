#include "tz.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

namespace {
Preferences tzPrefs;
int offsetMinutes = 0;
bool offsetKnown = false;
bool triedPrefsLoad = false;

bool parseUtcOffset(const String& json, int* outMinutes) {
  int keyPos = json.indexOf("\"utc_offset\"");
  if (keyPos < 0) return false;
  int colon = json.indexOf(':', keyPos + 12);
  if (colon < 0) return false;
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  if (i >= (int)json.length() || json[i] != '"') return false;
  i++;
  if (i >= (int)json.length()) return false;
  bool neg = (json[i] == '-');
  if (json[i] == '+' || json[i] == '-') i++;
  if (i + 4 >= (int)json.length()) return false;
  if (!isDigit(json[i]) || !isDigit(json[i + 1]) || !isDigit(json[i + 3]) || !isDigit(json[i + 4])) {
    return false;
  }
  int hh = (json[i] - '0') * 10 + (json[i + 1] - '0');
  int mm = (json[i + 3] - '0') * 10 + (json[i + 4] - '0');
  int total = hh * 60 + mm;
  *outMinutes = neg ? -total : total;
  return true;
}

void loadFromPrefsIfNeeded() {
  if (triedPrefsLoad) return;
  triedPrefsLoad = true;
  tzPrefs.begin("foxtz", true);
  if (tzPrefs.isKey("offset")) {
    offsetMinutes = tzPrefs.getInt("offset", 0);
    offsetKnown = true;
  }
  tzPrefs.end();
}
}

namespace FoxTz {
void refreshOffset() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  if (!http.begin(secureClient, "https://worldtimeapi.org/api/ip")) return;

  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    int minutes;
    if (parseUtcOffset(body, &minutes)) {
      offsetMinutes = minutes;
      offsetKnown = true;
      triedPrefsLoad = true;
      tzPrefs.begin("foxtz", false);
      tzPrefs.putInt("offset", minutes);
      tzPrefs.end();
    }
  }
  http.end();
}

bool hasOffset() {
  loadFromPrefsIfNeeded();
  return offsetKnown;
}

int getOffsetMinutes() {
  loadFromPrefsIfNeeded();
  return offsetMinutes;
}
}
