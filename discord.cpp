#include "discord.h"
#include "config.h"
#include "settings.h"
#include "profanity_filter.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <string.h>

namespace {

Preferences discordPrefs;
bool credsLoaded = false;
String botToken;
String channelId;

void loadCredsIfNeeded() {
  if (credsLoaded) return;
  discordPrefs.begin("foxdiscord", true);
  botToken = discordPrefs.getString("token", "");
  channelId = discordPrefs.getString("channel", "");
  discordPrefs.end();
  credsLoaded = true;
}

bool hasCreds() {
  loadCredsIfNeeded();
  return botToken.length() > 0 && channelId.length() > 0;
}

void doInit(const String& token, const String& channel) {
  discordPrefs.begin("foxdiscord", false);
  discordPrefs.putString("token", token);
  discordPrefs.putString("channel", channel);
  discordPrefs.end();
  botToken = token;
  channelId = channel;
  credsLoaded = true;
  Serial.println("OK");
}

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

unsigned long lastPostAttemptMs = 0;

void doPost(const String& message) {
  if (!hasCreds()) {
    Serial.println("ERROR:NOTINIT");
    return;
  }
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
  String url = "https://discord.com/api/v10/channels/" + channelId + "/messages";
  if (!http.begin(secureClient, url)) {
    Serial.println("ERROR:BADURL");
    return;
  }
  http.addHeader("Authorization", "Bot " + botToken);
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
  if (!hasCreds()) {
    Serial.println("ERROR:NOTINIT");
    return;
  }
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
  String url = "https://discord.com/api/v10/channels/" + channelId + "/messages?limit=" + String(limit);
  if (!http.begin(secureClient, url)) {
    Serial.println("ERROR:BADURL");
    return;
  }
  http.addHeader("Authorization", "Bot " + botToken);
  int code = http.GET();
  if (code != 200) {
    Serial.print("ERROR:HTTP:");
    Serial.println(code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  static String objects[DISCORD_READ_LIMIT_MAX];
  int count = splitJsonObjects(body, objects, DISCORD_READ_LIMIT_MAX);
  if (count == 0) {
    Serial.println("DISCORDREADDONE");
    return;
  }

  for (int i = count - 1; i >= 0; i--) {
    String content, timestamp;
    jsonExtractString(objects[i], "content", &content);
    jsonExtractString(objects[i], "timestamp", &timestamp);
    if ((int)content.length() > DISCORD_CONTENT_PREVIEW_MAX) {
      content = content.substring(0, DISCORD_CONTENT_PREVIEW_MAX) + "...";
    }
    String hhmm = (timestamp.length() >= 16) ? timestamp.substring(11, 16) : String("--:--");

    Serial.print("DISCORDMSG:");
    Serial.print(hhmm);
    Serial.print("|");
    Serial.println(content);
  }
  Serial.println("DISCORDREADDONE");
}

}

namespace FoxDiscord {

bool handleCommand(const String& line) {
  if (line.startsWith("DISCORDINIT:")) {
    String rest = line.substring(strlen("DISCORDINIT:"));
    int c = rest.indexOf(':');
    if (c < 0) {
      Serial.println("ERROR:BADFORMAT");
      return true;
    }
    String token = rest.substring(0, c);
    String channel = rest.substring(c + 1);
    if (token.length() == 0 || channel.length() == 0 ||
        (int)token.length() > DISCORD_TOKEN_MAX || (int)channel.length() > DISCORD_CHANNEL_ID_MAX) {
      Serial.println("ERROR:BADFORMAT");
      return true;
    }
    doInit(token, channel);
    return true;
  }

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
