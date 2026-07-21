#include "http_bridge.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <base64.h>

#include <WebSocketsClient.h>

namespace {

Preferences wifiPrefs;

WebSocketsClient wsClient;
bool wsActive = false;

bool parseWsUrl(const String& url, bool* isSecure, String* host, uint16_t* port, String* path) {
  String rest = url;
  if (rest.startsWith("wss://")) { *isSecure = true; rest = rest.substring(6); }
  else if (rest.startsWith("ws://")) { *isSecure = false; rest = rest.substring(5); }
  else return false;

  int slashPos = rest.indexOf('/');
  String hostPort = (slashPos < 0) ? rest : rest.substring(0, slashPos);
  *path = (slashPos < 0) ? "/" : rest.substring(slashPos);

  int colonPos = hostPort.indexOf(':');
  if (colonPos < 0) {
    *host = hostPort;
    *port = *isSecure ? 443 : 80;
  } else {
    *host = hostPort.substring(0, colonPos);
    *port = (uint16_t)hostPort.substring(colonPos + 1).toInt();
  }
  return host->length() > 0;
}

void wsEventHandler(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[SOCKET/CONNECTED]");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[SOCKET/DISCONNECTED]");
      break;
    case WStype_TEXT: {
      Serial.print("[SOCKET/MSG]");
      for (size_t i = 0; i < length; i++) Serial.write(payload[i]);
      Serial.println();
      break;
    }
    case WStype_BIN: {
      Serial.print("[SOCKET/MSG/BIN]");
      Serial.println(base64::encode(payload, length));
      break;
    }
    case WStype_ERROR:
      Serial.println("[SOCKET/ERROR]");
      break;
    default:
      break;
  }
}

int base64CharValue(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

String base64Decode(const String& input) {
  String out;
  out.reserve((input.length() / 4) * 3 + 3);
  int val = 0, bits = -8;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '=') break;
    int v = base64CharValue(c);
    if (v < 0) continue;
    val = (val << 6) | v;
    bits += 6;
    if (bits >= 0) {
      out += (char)((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return out;
}

bool jsonExtractString(const String& json, const String& key, String* out) {
  String pattern = "\"" + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) return false;
  int colon = json.indexOf(':', keyPos + pattern.length());
  if (colon < 0) return false;
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  if (i >= (int)json.length()) return false;

  if (json[i] == '"') {
    int end = i + 1;
    while (end < (int)json.length()) {
      if (json[end] == '"' && json[end - 1] != '\\') break;
      end++;
    }
    if (end >= (int)json.length()) return false;
    *out = json.substring(i + 1, end);
    return true;
  }

  int end = i;
  while (end < (int)json.length() && json[end] != ',' && json[end] != '}' &&
         json[end] != ' ' && json[end] != '\n') {
    end++;
  }
  *out = json.substring(i, end);
  return true;
}

String unquoteIfString(const String& s) {
  if (s.length() >= 2 && s[0] == '"' && s[s.length() - 1] == '"') {
    return s.substring(1, s.length() - 1);
  }
  return s;
}

bool jsonArrayExtract(const String& arrJson, int index, String* out) {
  if (index < 0) return false;
  int start = arrJson.indexOf('[');
  if (start < 0) return false;

  int i = start + 1;
  int elemIndex = 0;
  int depth = 0;
  bool inStr = false;
  int elemStart = i;
  int len = (int)arrJson.length();

  while (i < len) {
    char c = arrJson[i];
    if (inStr) {
      if (c == '\\' && i + 1 < len) { i += 2; continue; }
      if (c == '"') inStr = false;
      i++;
      continue;
    }
    if (c == '"') { inStr = true; i++; continue; }
    if (c == '[' || c == '{') { depth++; i++; continue; }
    if (c == ']' && depth == 0) {
      String elem = arrJson.substring(elemStart, i);
      elem.trim();
      if (elemIndex == index && elem.length() > 0) { *out = unquoteIfString(elem); return true; }
      return false;
    }
    if (c == ']' || c == '}') { depth--; i++; continue; }
    if (c == ',' && depth == 0) {
      String elem = arrJson.substring(elemStart, i);
      elem.trim();
      if (elemIndex == index) { *out = unquoteIfString(elem); return true; }
      elemIndex++;
      elemStart = i + 1;
      i++;
      continue;
    }
    i++;
  }
  return false;
}

void applyHeaders(HTTPClient& http, const String& json) {
  int hPos = json.indexOf("\"headers\"");
  if (hPos < 0) return;
  int braceStart = json.indexOf('{', hPos);
  if (braceStart < 0) return;

  int depth = 0, braceEnd = -1;
  for (int i = braceStart; i < (int)json.length(); i++) {
    if (json[i] == '{') depth++;
    else if (json[i] == '}') { depth--; if (depth == 0) { braceEnd = i; break; } }
  }
  if (braceEnd < 0) return;

  String body = json.substring(braceStart + 1, braceEnd);
  int pos = 0;
  while (pos < (int)body.length()) {
    int k1 = body.indexOf('"', pos);
    if (k1 < 0) break;
    int k2 = body.indexOf('"', k1 + 1);
    if (k2 < 0) break;
    String key = body.substring(k1 + 1, k2);
    int colon = body.indexOf(':', k2);
    if (colon < 0) break;
    int v1 = body.indexOf('"', colon);
    if (v1 < 0) break;
    int v2 = body.indexOf('"', v1 + 1);
    if (v2 < 0) break;
    String value = body.substring(v1 + 1, v2);
    http.addHeader(key, value);
    pos = v2 + 1;
  }
}

bool wifiSave(const String& ssid, const String& pass) {
  wifiPrefs.begin("foxwifi", false);
  int count = wifiPrefs.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    if (wifiPrefs.getString(("ssid" + String(i)).c_str(), "") == ssid) {
      wifiPrefs.putString(("pass" + String(i)).c_str(), pass);
      wifiPrefs.end();
      return true;
    }
  }
  if (count >= WIFI_SAVED_MAX) {
    wifiPrefs.end();
    return false;
  }
  wifiPrefs.putString(("ssid" + String(count)).c_str(), ssid);
  wifiPrefs.putString(("pass" + String(count)).c_str(), pass);
  wifiPrefs.putInt("count", count + 1);
  wifiPrefs.end();
  return true;
}

bool tryConnect(const String& ssid, const String& pass) {
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void handleWifiConnect(const String& json) {
  String ssid, pass;
  bool hasSsid = jsonExtractString(json, "ssid", &ssid);
  bool hasPass = jsonExtractString(json, "password", &pass);

  if (!hasSsid) {
    wifiPrefs.begin("foxwifi", true);
    int count = wifiPrefs.getInt("count", 0);
    wifiPrefs.end();
    if (count == 0) {
      Serial.println("[ERROR] no saved networks");
      return;
    }
    for (int i = 0; i < count; i++) {
      wifiPrefs.begin("foxwifi", true);
      String s = wifiPrefs.getString(("ssid" + String(i)).c_str(), "");
      String p = wifiPrefs.getString(("pass" + String(i)).c_str(), "");
      wifiPrefs.end();
      if (tryConnect(s, p)) {
        Serial.print("[WIFI/CONNECT/SUCCESS]");
        Serial.println(s);
        return;
      }
    }
    Serial.println("[ERROR] could not connect to any saved network");
    return;
  }

  if (!hasPass) {
    wifiPrefs.begin("foxwifi", true);
    int count = wifiPrefs.getInt("count", 0);
    for (int i = 0; i < count; i++) {
      if (wifiPrefs.getString(("ssid" + String(i)).c_str(), "") == ssid) {
        pass = wifiPrefs.getString(("pass" + String(i)).c_str(), "");
        break;
      }
    }
    wifiPrefs.end();
  }

  if (tryConnect(ssid, pass)) {
    Serial.print("[WIFI/CONNECT/SUCCESS]");
    Serial.println(ssid);
  } else {

    Serial.print("[ERROR] failed to connect (status=");
    Serial.print((int)WiFi.status());
    Serial.println(")");
  }
}

bool wifiForget(const String& ssid) {
  wifiPrefs.begin("foxwifi", false);
  int count = wifiPrefs.getInt("count", 0);
  int found = -1;
  for (int i = 0; i < count; i++) {
    if (wifiPrefs.getString(("ssid" + String(i)).c_str(), "") == ssid) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    wifiPrefs.end();
    return false;
  }

  for (int i = found; i < count - 1; i++) {
    wifiPrefs.putString(("ssid" + String(i)).c_str(), wifiPrefs.getString(("ssid" + String(i + 1)).c_str(), ""));
    wifiPrefs.putString(("pass" + String(i)).c_str(), wifiPrefs.getString(("pass" + String(i + 1)).c_str(), ""));
  }
  wifiPrefs.remove(("ssid" + String(count - 1)).c_str());
  wifiPrefs.remove(("pass" + String(count - 1)).c_str());
  wifiPrefs.putInt("count", count - 1);
  wifiPrefs.end();
  return true;
}

void handleWifiForget(const String& json) {
  String ssid;
  if (!jsonExtractString(json, "ssid", &ssid)) {
    Serial.println("[ERROR] missing ssid");
    return;
  }
  if (wifiForget(ssid)) {
    Serial.println("[WIFI/FORGET/SUCCESS]");
  } else {
    Serial.println("[ERROR] network not found");
  }
}

void handleWifiSave(const String& json) {
  String ssid, pass;
  if (!jsonExtractString(json, "ssid", &ssid)) {
    Serial.println("[ERROR] missing ssid");
    return;
  }
  jsonExtractString(json, "password", &pass);
  if (wifiSave(ssid, pass)) {
    Serial.println("[WIFI/SAVE/SUCCESS]");
  } else {
    Serial.println("[ERROR] saved network list full");
  }
}

void handleWifiList() {
  wifiPrefs.begin("foxwifi", true);
  int count = wifiPrefs.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    Serial.print("[WIFI/LIST]");
    Serial.println(wifiPrefs.getString(("ssid" + String(i)).c_str(), ""));
  }
  wifiPrefs.end();
  Serial.println("[WIFI/LIST/SUCCESS]");
}

void handleWifiScan() {
  int found = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < found; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  Serial.print("[WIFI/SCAN/SUCCESS]");
  Serial.println(json);
}

void handleWifiAp(const String& json) {
  String ssid, pass;
  if (!jsonExtractString(json, "ssid", &ssid)) {
    Serial.println("[ERROR] missing ssid");
    return;
  }
  jsonExtractString(json, "password", &pass);
  bool ok = pass.length() >= 8 ? WiFi.softAP(ssid.c_str(), pass.c_str())
                                : WiFi.softAP(ssid.c_str());
  if (ok) {
    Serial.print("[WIFI/AP/SUCCESS]");
    Serial.println(WiFi.softAPIP().toString());
  } else {
    Serial.println("[ERROR] failed to start AP");
  }
}

void doHttpRequest(const String& method, const String& url, const String& payload,
                    const String& rawJson, const String& successTag) {
  if (url.length() == 0) {
    Serial.println("[ERROR] missing url");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] not connected to WiFi");
    return;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  bool began;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    began = http.begin(secureClient, url);
  } else {
    began = http.begin(plainClient, url);
  }
  if (!began) {
    Serial.println("[ERROR] invalid url");
    return;
  }
  applyHeaders(http, rawJson);

  int code;
  if (method == "GET") code = http.GET();
  else if (method == "POST") code = http.POST(payload);
  else if (method == "PUT") code = http.PUT(payload);
  else if (method == "PATCH") code = http.PATCH(payload);
  else if (method == "DELETE") code = http.sendRequest("DELETE", payload);
  else code = -1;

  if (code <= 0) {
    Serial.print("[ERROR] request failed: ");
    Serial.println(http.errorToString(code));
    http.end();
    return;
  }

  String body = http.getString();
  if ((int)body.length() > HTTP_BODY_MAX) {
    body = body.substring(0, HTTP_BODY_MAX);
  }
  Serial.print(successTag);
  Serial.print(code);
  Serial.print(" ");
  Serial.println(body);
  http.end();
}

void handlePlainGet(const String& url) {
  doHttpRequest("GET", url, "", "", "[GET/SUCCESS]");
}

void doHttpRequestBytes(const String& method, const String& url, const String& payloadB64,
                         const String& rawJson, const String& successTag) {
  if (url.length() == 0) {
    Serial.println("[ERROR] missing url");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] not connected to WiFi");
    return;
  }

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  bool began;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    began = http.begin(secureClient, url);
  } else {
    began = http.begin(plainClient, url);
  }
  if (!began) {
    Serial.println("[ERROR] invalid url");
    return;
  }
  applyHeaders(http, rawJson);

  int code;
  if (method == "GET") {
    code = http.GET();
  } else if (method == "POST") {
    String decoded = payloadB64.length() > 0 ? base64Decode(payloadB64) : String("");
    code = http.POST((uint8_t*)decoded.c_str(), decoded.length());
  } else {
    code = -1;
  }

  if (code <= 0) {
    Serial.print("[ERROR] request failed: ");
    Serial.println(http.errorToString(code));
    http.end();
    return;
  }

  static uint8_t buf[HTTP_BYTES_MAX];
  WiFiClient* stream = http.getStreamPtr();
  int totalLen = http.getSize();
  int toRead = (totalLen > 0) ? min(totalLen, HTTP_BYTES_MAX) : HTTP_BYTES_MAX;
  int readLen = 0;
  unsigned long start = millis();
  while (http.connected() && readLen < toRead && millis() - start < (unsigned long)HTTP_TIMEOUT_MS) {
    int avail = stream->available();
    if (avail > 0) {
      int want = min(avail, toRead - readLen);
      int got = stream->readBytes(buf + readLen, want);
      readLen += got;
    } else {
      delay(1);
    }
  }

  Serial.print(successTag);
  Serial.print(code);
  Serial.print(" ");
  Serial.println(base64::encode(buf, readLen));
  http.end();
}

void handleGetBytes(const String& url) {
  doHttpRequestBytes("GET", url, "", "", "[GET/BYTES/SUCCESS]");
}

void handlePostBytes(const String& json) {
  String url, payloadB64;
  jsonExtractString(json, "url", &url);
  jsonExtractString(json, "payload_b64", &payloadB64);
  doHttpRequestBytes("POST", url, payloadB64, json, "[POST/BYTES/SUCCESS]");
}

void handleParse(const String& json) {
  String source, key;
  if (!jsonExtractString(json, "json", &source) || !jsonExtractString(json, "key", &key)) {
    Serial.println("[ERROR] missing json/key");
    return;
  }
  String value;
  if (jsonExtractString(source, key, &value)) {
    Serial.print("[PARSE/SUCCESS]");
    Serial.println(value);
  } else {
    Serial.println("[ERROR] key not found");
  }
}

void handleSocketStart(const String& json) {
  String url;
  if (!jsonExtractString(json, "url", &url)) {
    Serial.println("[ERROR] missing url");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] not connected to WiFi");
    return;
  }

  bool secure;
  String host, path;
  uint16_t port;
  if (!parseWsUrl(url, &secure, &host, &port, &path)) {
    Serial.println("[ERROR] invalid url - expected ws:// or wss://");
    return;
  }

  wsClient.onEvent(wsEventHandler);
  wsClient.setReconnectInterval(WEBSOCKET_RECONNECT_MS);
  if (secure) {
    wsClient.beginSSL(host.c_str(), port, path.c_str());
  } else {
    wsClient.begin(host.c_str(), port, path.c_str());
  }
  wsActive = true;
  Serial.println("[SOCKET/START/SUCCESS]");
}

void handleSocketStop() {
  wsClient.disconnect();
  wsActive = false;
  Serial.println("[SOCKET/STOP/SUCCESS]");
}

void handleSocketSend(const String& msg) {
  if (!wsActive) {
    Serial.println("[ERROR] socket not started");
    return;
  }

  String payload = msg;
  wsClient.sendTXT(payload);
  Serial.println("[SOCKET/SEND/SUCCESS]");
}

void handleParseArray(const String& json) {
  String source, idxStr;
  if (!jsonExtractString(json, "json", &source) || !jsonExtractString(json, "index", &idxStr)) {
    Serial.println("[ERROR] missing json/index");
    return;
  }
  String value;
  if (jsonArrayExtract(source, idxStr.toInt(), &value)) {
    Serial.print("[PARSE/ARRAY/SUCCESS]");
    Serial.println(value);
  } else {
    Serial.println("[ERROR] index not found");
  }
}

}

namespace FoxHttp {

void begin() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
}

void loop() {
  if (wsActive) wsClient.loop();
}

bool handleCommand(const String& line) {
  if (!line.startsWith("[")) return false;

  int closeBracket = line.indexOf(']');
  if (closeBracket < 0) return false;

  String cmd = line.substring(1, closeBracket);
  String rest = line.substring(closeBracket + 1);
  rest.trim();
  cmd.toUpperCase();

  if (cmd == "PING") { Serial.println("[PONG]"); return true; }

  if (cmd == "LIST") {
    Serial.println("[LIST/SUCCESS]");
    static const char* cmds[] = {
      "[PING]", "[LIST]", "[REBOOT]", "[VERSION]",
      "[WIFI/SCAN]", "[WIFI/SAVE]", "[WIFI/CONNECT]", "[WIFI/DISCONNECT]",
      "[WIFI/FORGET]",
      "[WIFI/LIST]", "[WIFI/IP]", "[WIFI/SSID]", "[WIFI/STATUS]",
      "[IP/ADDRESS]", "[WIFI/AP]",
      "[GET]", "[GET/HTTP]", "[POST/HTTP]", "[PUT/HTTP]", "[PATCH/HTTP]", "[DELETE/HTTP]",
      "[GET/BYTES]", "[POST/BYTES]",
      "[SOCKET/START]", "[SOCKET/SEND]", "[SOCKET/STOP]",
      "[PARSE]", "[PARSE/ARRAY]", "[LED/ON]", "[LED/OFF]"
    };
    for (auto c : cmds) Serial.println(c);
    return true;
  }

  if (cmd == "REBOOT") { Serial.println("[REBOOT/SUCCESS]"); delay(100); ESP.restart(); return true; }

  if (cmd == "VERSION") {
    Serial.print("[VERSION/SUCCESS]");
    Serial.println(FOX_FIRMWARE_VERSION);
    return true;
  }

  if (cmd == "WIFI/SCAN") { handleWifiScan(); return true; }
  if (cmd == "WIFI/SAVE") { handleWifiSave(rest); return true; }
  if (cmd == "WIFI/CONNECT") { handleWifiConnect(rest); return true; }
  if (cmd == "WIFI/DISCONNECT") { WiFi.disconnect(); Serial.println("[WIFI/DISCONNECT/SUCCESS]"); return true; }
  if (cmd == "WIFI/FORGET") { handleWifiForget(rest); return true; }
  if (cmd == "WIFI/LIST") { handleWifiList(); return true; }

  if (cmd == "WIFI/STATUS") {
    Serial.print("[WIFI/STATUS/SUCCESS]");
    Serial.println(WiFi.status() == WL_CONNECTED ? "true" : "false");
    return true;
  }

  if (cmd == "WIFI/SSID") {
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[ERROR] not connected"); return true; }
    Serial.print("[WIFI/SSID/SUCCESS]");
    Serial.println(WiFi.SSID());
    return true;
  }

  if (cmd == "WIFI/IP") {
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[ERROR] not connected"); return true; }
    doHttpRequest("GET", "http://api.ipify.org", "", "", "[WIFI/IP/SUCCESS]");
    return true;
  }

  if (cmd == "IP/ADDRESS") {
    Serial.print("[IP/ADDRESS/SUCCESS]");
    Serial.println(WiFi.localIP().toString());
    return true;
  }

  if (cmd == "WIFI/AP") { handleWifiAp(rest); return true; }

  if (cmd == "GET") { handlePlainGet(rest); return true; }
  if (cmd == "GET/BYTES") { handleGetBytes(rest); return true; }
  if (cmd == "POST/BYTES") { handlePostBytes(rest); return true; }

  if (cmd == "GET/HTTP") {
    String url; jsonExtractString(rest, "url", &url);
    doHttpRequest("GET", url, "", rest, "[GET/HTTP/SUCCESS]");
    return true;
  }
  if (cmd == "POST/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("POST", url, payload, rest, "[POST/HTTP/SUCCESS]");
    return true;
  }
  if (cmd == "PUT/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("PUT", url, payload, rest, "[PUT/HTTP/SUCCESS]");
    return true;
  }
  if (cmd == "PATCH/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("PATCH", url, payload, rest, "[PATCH/HTTP/SUCCESS]");
    return true;
  }
  if (cmd == "DELETE/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("DELETE", url, payload, rest, "[DELETE/HTTP/SUCCESS]");
    return true;
  }

  if (cmd == "PARSE") { handleParse(rest); return true; }
  if (cmd == "PARSE/ARRAY") { handleParseArray(rest); return true; }

  if (cmd == "SOCKET/START") { handleSocketStart(rest); return true; }
  if (cmd == "SOCKET/STOP") { handleSocketStop(); return true; }
  if (cmd == "SOCKET/SEND") { handleSocketSend(rest); return true; }

  if (cmd == "LED/ON") { digitalWrite(STATUS_LED_PIN, HIGH); Serial.println("[LED/ON/SUCCESS]"); return true; }
  if (cmd == "LED/OFF") { digitalWrite(STATUS_LED_PIN, LOW); Serial.println("[LED/OFF/SUCCESS]"); return true; }

  Serial.println("[ERROR] unknown command");
  return true;
}

}
