#include "discord.h"
#include "config.h"
#include "settings.h"
#include "profanity_filter.h"
#include "tz.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>

namespace {

bool jsonExtractString(const String& json, const String& key, String* out) {
  String pattern = "\"" + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) return false;
  int colon = json.indexOf(':', keyPos + pattern.length());
  if (colon < 0) return false;
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  if (i >= (int)json.length() || json[i] != '"') return false;
  int end = i + 1;
  while (end < (int)json.length()) {
    if (json[end] == '"' && json[end - 1] != '\\') break;
    end++;
  }
  if (end >= (int)json.length()) return false;
  *out = json.substring(i + 1, end);
  return true;
}

int splitJsonObjects(const String& arr, String* out, int maxCount) {
  int start = arr.indexOf('[');
  if (start < 0) return 0;
  int i = start + 1;
  int len = (int)arr.length();
  int depth = 0;
  bool inStr = false;
  int elemStart = -1;
  int count = 0;

  while (i < len && count < maxCount) {
    char c = arr[i];
    if (inStr) {
      if (c == '\\' && i + 1 < len) { i += 2; continue; }
      if (c == '"') inStr = false;
      i++;
      continue;
    }
    if (c == '"') { inStr = true; i++; continue; }
    if (c == '{') {
      if (depth == 0) elemStart = i;
      depth++;
      i++;
      continue;
    }
    if (c == '}') {
      depth--;
      if (depth == 0 && elemStart >= 0) {
        out[count++] = arr.substring(elemStart, i + 1);
        elemStart = -1;
      }
      i++;
      continue;
    }
    if (c == ']' && depth == 0) break;
    i++;
  }
  return count;
}

struct SimpleDT {
  int year;
  int mon;
  int day;
  int hour;
  int min;
};

int daysInMonth(int year, int month) {
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return leap ? 29 : 28;
  }
  if (month < 1 || month > 12) return 30;
  return dim[month - 1];
}

void applyOffset(SimpleDT* dt, int offsetMinutes) {
  int total = dt->hour * 60 + dt->min + offsetMinutes;
  int dayDelta = 0;
  while (total < 0) {
    total += 1440;
    dayDelta--;
  }
  while (total >= 1440) {
    total -= 1440;
    dayDelta++;
  }
  dt->hour = total / 60;
  dt->min = total % 60;
  dt->day += dayDelta;
  if (dt->day < 1) {
    dt->mon--;
    if (dt->mon < 1) {
      dt->mon = 12;
      dt->year--;
    }
    dt->day = daysInMonth(dt->year, dt->mon);
  } else {
    int dim = daysInMonth(dt->year, dt->mon);
    if (dt->day > dim) {
      dt->day = 1;
      dt->mon++;
      if (dt->mon > 12) {
        dt->mon = 1;
        dt->year++;
      }
    }
  }
}

bool isAllDigits(const String& s, int start, int len) {
  if (start < 0 || start + len > (int)s.length()) return false;
  for (int k = 0; k < len; k++) {
    if (!isDigit(s[start + k])) return false;
  }
  return true;
}

bool parseIsoDt(const String& ts, SimpleDT* out) {
  if (ts.length() < 16) return false;
  if (!isAllDigits(ts, 0, 4) || !isAllDigits(ts, 5, 2) || !isAllDigits(ts, 8, 2) ||
      !isAllDigits(ts, 11, 2) || !isAllDigits(ts, 14, 2)) {
    return false;
  }
  out->year = ts.substring(0, 4).toInt();
  out->mon = ts.substring(5, 7).toInt();
  out->day = ts.substring(8, 10).toInt();
  out->hour = ts.substring(11, 13).toInt();
  out->min = ts.substring(14, 16).toInt();
  return true;
}

int monthFromAbbrev(const String& abbr) {
  static const char* names[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int k = 0; k < 12; k++) {
    if (abbr.equalsIgnoreCase(names[k])) return k + 1;
  }
  return 0;
}

const char* monthAbbrev(int mon) {
  static const char* names[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (mon < 1 || mon > 12) return "???";
  return names[mon - 1];
}

bool parseHttpDate(const String& d, SimpleDT* out) {
  if (d.length() < 25) return false;
  if (!isAllDigits(d, 5, 2) || !isAllDigits(d, 12, 4) || !isAllDigits(d, 17, 2) ||
      !isAllDigits(d, 20, 2)) {
    return false;
  }
  int mon = monthFromAbbrev(d.substring(8, 11));
  if (mon == 0) return false;
  out->day = d.substring(5, 7).toInt();
  out->mon = mon;
  out->year = d.substring(12, 16).toInt();
  out->hour = d.substring(17, 19).toInt();
  out->min = d.substring(20, 22).toInt();
  return true;
}

unsigned long lastPostAttemptMs = 0;

void doPost(const String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR:NOWIFI");
    return;
  }

  if (FoxSettings::profanityFilterEnabled() && FoxProfanityFilter::containsBlockedContent(message)) {
    Serial.println("ERROR:PROFANITY");
    return;
  }

  unsigned long now = millis();
  if (lastPostAttemptMs != 0 && (now - lastPostAttemptMs) < DISCORD_POST_MIN_INTERVAL_MS) {
    Serial.println("ERROR:RATELIMIT");
    return;
  }
  lastPostAttemptMs = now;

  String escaped;
  escaped.reserve(message.length() + 8);
  for (size_t i = 0; i < message.length(); i++) {
    char c = message[i];
    if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') {  }
    else escaped += c;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  String url = String(FOXCHAT_RELAY_BASE_URL) + "/post";
  if (!http.begin(secureClient, url)) {
    Serial.println("ERROR:BADURL");
    return;
  }
  http.addHeader("X-App-Key", FOXCHAT_RELAY_APP_KEY);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"content\":\"" + escaped + "\",\"allowed_mentions\":{\"parse\":[]}}";
  int code = http.POST(payload);
  http.end();

  if (code == 200 || code == 201) {
    Serial.println("OK");
  } else {
    Serial.print("ERROR:HTTP:");
    Serial.println(code);
  }
}

void doRead(int limit) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR:NOWIFI");
    return;
  }
  if (limit < 1) limit = DISCORD_READ_LIMIT_DEFAULT;
  if (limit > DISCORD_READ_LIMIT_MAX) limit = DISCORD_READ_LIMIT_MAX;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  String url = String(FOXCHAT_RELAY_BASE_URL) + "/read?limit=" + String(limit);
  if (!http.begin(secureClient, url)) {
    Serial.println("ERROR:BADURL");
    return;
  }
  http.addHeader("X-App-Key", FOXCHAT_RELAY_APP_KEY);
  const char* headerKeys[] = {"Date"};
  http.collectHeaders(headerKeys, 1);
  int code = http.GET();
  if (code != 200) {
    Serial.print("ERROR:HTTP:");
    Serial.println(code);
    http.end();
    return;
  }
  String dateHeader = http.header("Date");
  String body = http.getString();
  http.end();

  static String objects[DISCORD_READ_LIMIT_MAX];
  int count = splitJsonObjects(body, objects, DISCORD_READ_LIMIT_MAX);
  if (count == 0) {
    Serial.println("DISCORDREADDONE");
    return;
  }

  int offsetMinutes = FoxTz::getOffsetMinutes();
  bool tzKnown = FoxTz::hasOffset();
  SimpleDT nowLocal;
  bool haveNow = parseHttpDate(dateHeader, &nowLocal);
  if (haveNow) applyOffset(&nowLocal, offsetMinutes);

  for (int i = count - 1; i >= 0; i--) {
    String content, timestamp;
    jsonExtractString(objects[i], "content", &content);
    jsonExtractString(objects[i], "timestamp", &timestamp);
    if ((int)content.length() > DISCORD_CONTENT_PREVIEW_MAX) {
      content = content.substring(0, DISCORD_CONTENT_PREVIEW_MAX) + "...";
    }

    char timeField[16];
    char fullTimeField[24];
    SimpleDT msgLocal;
    if (parseIsoDt(timestamp, &msgLocal)) {
      applyOffset(&msgLocal, offsetMinutes);
      bool sameDay = haveNow && msgLocal.year == nowLocal.year && msgLocal.mon == nowLocal.mon &&
                     msgLocal.day == nowLocal.day;
      char tag = tzKnown ? 'L' : 'Z';
      if (sameDay || !haveNow) {
        snprintf(timeField, sizeof(timeField), "%c%02d:%02d", tag, msgLocal.hour, msgLocal.min);
      } else {
        snprintf(
            timeField, sizeof(timeField), "%c%02d/%02d %02d:%02d", tag, msgLocal.day, msgLocal.mon,
            msgLocal.hour, msgLocal.min);
      }
      snprintf(
          fullTimeField, sizeof(fullTimeField), "%c%02d %s %04d, %02d:%02d", tag, msgLocal.day,
          monthAbbrev(msgLocal.mon), msgLocal.year, msgLocal.hour, msgLocal.min);
    } else {
      snprintf(timeField, sizeof(timeField), "Z--:--");
      snprintf(fullTimeField, sizeof(fullTimeField), "Zunknown time");
    }

    Serial.print("DISCORDMSG:");
    Serial.print(timeField);
    Serial.print("|");
    Serial.print(fullTimeField);
    Serial.print("|");
    Serial.println(content);
  }
  Serial.println("DISCORDREADDONE");
}
}

namespace FoxDiscord {
bool handleCommand(const String& line) {
  if (line.startsWith("DISCORDPOST:")) {
    doPost(line.substring(strlen("DISCORDPOST:")));
    return true;
  }

  if (line == "DISCORDREAD") {
    doRead(DISCORD_READ_LIMIT_DEFAULT);
    return true;
  }

  if (line.startsWith("DISCORDREAD:")) {
    doRead(line.substring(strlen("DISCORDREAD:")).toInt());
    return true;
  }

  return false;
}
}
