#include "gemini.h"
#include "config.h"

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
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n')) i++;
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

String jsonEscape(const String& text) {
  String escaped;
  escaped.reserve(text.length() + 8);
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') {  }
    else escaped += c;
  }
  return escaped;
}

unsigned long lastAskAttemptMs = 0;

unsigned long cooldownRemainingSec() {
  if (lastAskAttemptMs == 0) return 0;
  unsigned long cooldownMs = (unsigned long)GEMINI_RATELIMIT_COOLDOWN_SEC * 1000UL;
  unsigned long elapsed = millis() - lastAskAttemptMs;
  if (elapsed >= cooldownMs) return 0;
  return (cooldownMs - elapsed + 999) / 1000;
}

void doAsk(const String& prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR:NOWIFI");
    return;
  }

  if (prompt.length() == 0) {
    Serial.println("ERROR:EMPTY");
    return;
  }

  unsigned long remaining = cooldownRemainingSec();
  if (remaining > 0) {
    Serial.print("ERROR:RATELIMIT:");
    Serial.println(remaining);
    return;
  }
  lastAskAttemptMs = millis();

  HTTPClient http;
  http.setTimeout(GEMINI_API_TIMEOUT_MS);
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  String url = String(GEMINI_RELAY_BASE_URL) + "/ask";
  if (!http.begin(secureClient, url)) {
    Serial.println("ERROR:BADURL");
    return;
  }
  http.addHeader("X-App-Key", GEMINI_RELAY_APP_KEY);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"prompt\":\"" + jsonEscape(prompt) + "\"}";
  int code = http.POST(payload);

  if (code != 200) {
    Serial.print("ERROR:HTTP:");
    Serial.println(code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  String text;
  if (jsonExtractString(body, "text", &text)) {
    Serial.print("AIREPLY:");
    Serial.println(text);
    return;
  }

  String blockReason;
  if (jsonExtractString(body, "blockReason", &blockReason)) {
    Serial.println("ERROR:BLOCKED");
    return;
  }

  Serial.println("ERROR:NOREPLY");
}

void doCooldown() {
  Serial.print("COOLDOWN:");
  Serial.println(cooldownRemainingSec());
}

}

namespace FoxGemini {
bool handleCommand(const String& line) {
  if (line == "AICOOLDOWN") {
    doCooldown();
    return true;
  }

  if (line.startsWith("AIASK:")) {
    doAsk(line.substring(strlen("AIASK:")));
    return true;
  }

  return false;
}
}
