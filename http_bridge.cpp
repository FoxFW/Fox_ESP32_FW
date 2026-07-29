#include "http_bridge.h"
#include "config.h"
#include "tz.h"

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

bool extractBalanced(const String& json, const String& key, char openCh, char closeCh, String* out) {
  String pattern = "\"" + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) return false;
  int start = json.indexOf(openCh, keyPos + pattern.length());
  if (start < 0) return false;

  int depth = 0;
  bool inStr = false;
  for (int i = start; i < (int)json.length(); i++) {
    char c = json[i];
    if (inStr) {
      if (c == '\\' && i + 1 < (int)json.length()) { i++; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == openCh) {
      depth++;
    } else if (c == closeCh) {
      depth--;
      if (depth == 0) { *out = json.substring(start, i + 1); return true; }
    }
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
        Serial.println("[SUCCESS] Connected to Wifi.");
        FoxTz::refreshOffset();
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
    Serial.println("[SUCCESS] Connected to Wifi.");
    FoxTz::refreshOffset();
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
    Serial.println("[SUCCESS] Wifi settings saved.");
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

void handleWifiSavedList() {
  wifiPrefs.begin("foxwifi", true);
  int count = wifiPrefs.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    String s = wifiPrefs.getString(("ssid" + String(i)).c_str(), "");
    String p = wifiPrefs.getString(("pass" + String(i)).c_str(), "");
    Serial.print("[WIFI/SAVED/LIST]{\"ssid\":\"");
    Serial.print(s);
    Serial.print("\",\"password\":\"");
    Serial.print(p);
    Serial.println("\"}");
  }
  wifiPrefs.end();
  Serial.println("[WIFI/SAVED/LIST/SUCCESS]");
}

void handleWifiScan() {
  int found = WiFi.scanNetworks();

  String namesJson = "[";
  String detailsJson = "[";
  for (int i = 0; i < found; i++) {
    if (i > 0) { namesJson += ","; detailsJson += ","; }
    namesJson += "\"" + WiFi.SSID(i) + "\"";
    detailsJson += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                    ",\"secure\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  namesJson += "]";
  detailsJson += "]";
  WiFi.scanDelete();

  String body = "{\"networks\":" + namesJson + ",\"details\":" + detailsJson + "}";
  Serial.println("[GET/SUCCESS]");
  Serial.println(body);
  Serial.flush();
  Serial.println();
  Serial.println("[GET/END]");
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

struct ChunkedRead {
  bool chunked;
  uint32_t remaining;
  bool needSize;
  bool done;
  char sizeLineBuf[24];
  uint8_t sizeLineLen;
  uint8_t crlfSkip;
};

void chunkedReadInit(ChunkedRead* cr, HTTPClient& http) {
  cr->chunked = (http.getSize() < 0);
  cr->remaining = 0;
  cr->needSize = true;
  cr->done = false;
  cr->sizeLineLen = 0;
  cr->crlfSkip = 0;
}

static bool chunkedReadSizeLine(ChunkedRead* cr, WiFiClient* stream) {
  while (stream->available() > 0) {
    int c = stream->read();
    if (c < 0) break;
    if (c == '\n') {
      cr->sizeLineBuf[cr->sizeLineLen] = '\0';
      char line[sizeof(cr->sizeLineBuf)];
      memcpy(line, cr->sizeLineBuf, cr->sizeLineLen + 1);
      cr->sizeLineLen = 0;

      char* trimmed = line;
      while (*trimmed == ' ') trimmed++;
      size_t tlen = strlen(trimmed);
      while (tlen > 0 && (trimmed[tlen - 1] == '\r' || trimmed[tlen - 1] == ' ')) {
        trimmed[--tlen] = '\0';
      }
      if (*trimmed == '\0') continue;

      char* endPtr = NULL;
      long size = strtol(trimmed, &endPtr, 16);
      if (endPtr == trimmed) continue;

      if (size <= 0) {
        cr->done = true;
        return true;
      }
      cr->remaining = (uint32_t)size;
      cr->needSize = false;
      return true;
    }
    if (c != '\r' && (size_t)cr->sizeLineLen + 1 < sizeof(cr->sizeLineBuf)) {
      cr->sizeLineBuf[cr->sizeLineLen++] = (char)c;
    }
  }
  return false;
}

int chunkedReadNext(ChunkedRead* cr, WiFiClient* stream, char* out, int maxLen) {
  if (!cr->chunked) {
    int avail = stream->available();
    if (avail <= 0) return 0;
    int want = avail < maxLen ? avail : maxLen;
    int got = stream->readBytes(out, want);
    return got > 0 ? got : 0;
  }
  if (cr->done) return 0;
  if (cr->crlfSkip > 0) {
    while (cr->crlfSkip > 0 && stream->available() > 0) {
      stream->read();
      cr->crlfSkip--;
    }
    if (cr->crlfSkip > 0) return 0;
  }
  if (cr->needSize) {
    chunkedReadSizeLine(cr, stream);
    if (cr->needSize) return 0;
    if (cr->done) return 0;
  }
  if (cr->remaining == 0) return 0;
  int want = (int)(cr->remaining < (uint32_t)maxLen ? cr->remaining : (uint32_t)maxLen);
  int got = stream->readBytes(out, want);
  if (got <= 0) return 0;
  cr->remaining -= (uint32_t)got;
  if (cr->remaining == 0) {
    cr->crlfSkip = 2;
    cr->needSize = true;
  }
  return got;
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

  String body;
  {
    WiFiClient* stream = http.getStreamPtr();
    ChunkedRead cr;
    chunkedReadInit(&cr, http);
    char buf[256];
    unsigned long start = millis();
    while ((http.connected() || stream->available()) &&
           (millis() - start) < (unsigned long)HTTP_TIMEOUT_MS) {
      int got = chunkedReadNext(&cr, stream, buf, sizeof(buf));
      if (got > 0) {
        if ((int)body.length() < HTTP_BODY_MAX) body.concat(buf, got);
        start = millis();
      } else {
        if (cr.done) break;
        delay(1);
      }
    }
  }
  if ((int)body.length() > HTTP_BODY_MAX) {
    body = body.substring(0, HTTP_BODY_MAX);
  }

  Serial.print(successTag);
  Serial.print("{\"Status-Code\":");
  Serial.print(code);
  Serial.print(",\"Content-Length\":");
  Serial.print(body.length());
  Serial.println("}");
  Serial.println(body);
  Serial.flush();
  Serial.println();

  String endTag = "[HTTP/END]";
  if (method == "GET") endTag = "[GET/END]";
  else if (method == "POST") endTag = "[POST/END]";
  else if (method == "PUT") endTag = "[PUT/END]";
  else if (method == "PATCH") endTag = "[PATCH/END]";
  else if (method == "DELETE") endTag = "[DELETE/END]";
  Serial.println(endTag);
  http.end();
}

void handlePlainGet(const String& url) {
  doHttpRequest("GET", url, "", "", "[GET/SUCCESS]");
}

void handleWifiIp() {
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  if (!http.begin(secureClient, "https://httpbin.org/get")) {
    Serial.println("[ERROR] Unable to connect to the server.");
    return;
  }
  int code = http.GET();
  if (code <= 0) {
    Serial.print("[ERROR] GET Request Failed, error: ");
    Serial.println(http.errorToString(code));
    http.end();
    return;
  }
  String body;
  {
    WiFiClient* stream = http.getStreamPtr();
    ChunkedRead cr;
    chunkedReadInit(&cr, http);
    char buf[256];
    unsigned long start = millis();
    while ((http.connected() || stream->available()) &&
           (millis() - start) < (unsigned long)HTTP_TIMEOUT_MS) {
      int got = chunkedReadNext(&cr, stream, buf, sizeof(buf));
      if (got > 0) {
        body.concat(buf, got);
        start = millis();
      } else {
        if (cr.done) break;
        delay(1);
      }
    }
  }
  http.end();

  if (body.length() == 0) {
    Serial.println("[ERROR] GET request failed or returned empty data.");
    return;
  }
  String origin;
  if (!jsonExtractString(body, "origin", &origin)) {
    Serial.println("[ERROR] JSON does not contain origin.");
    return;
  }

  Serial.print("[GET/SUCCESS]{\"Status-Code\":");
  Serial.print(code);
  Serial.print(",\"Content-Length\":");
  Serial.print(origin.length());
  Serial.println("}");
  Serial.println(origin);
  Serial.flush();
  Serial.println();
  Serial.println("[GET/END]");
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
    Serial.println(value);
  } else {
    Serial.println("[ERROR] Key not found in JSON.");
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
  Serial.println("[SOCKET/STOPPED]");
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
    Serial.println(value);
  } else {
    Serial.println("[ERROR] Key not found in JSON.");
  }
}

WiFiClientSecure dlSecureClient;
WiFiClient dlPlainClient;
HTTPClient dlHttp;
bool dlActive = false;
int dlTotalSize = -1;
int dlBytesRead = 0;

void dlCleanup() {
  if (dlActive) {
    dlHttp.end();
  }
  dlActive = false;
  dlTotalSize = -1;
  dlBytesRead = 0;
}

void handleDownloadStart(const String& json) {
  String url;
  if (!jsonExtractString(json, "url", &url)) {
    Serial.println("[ERROR] missing url");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] not connected to WiFi");
    return;
  }
  dlCleanup();

  dlHttp.setTimeout(HTTP_TIMEOUT_MS);
  dlHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  bool began;
  if (url.startsWith("https://")) {
    dlSecureClient.setInsecure();
    began = dlHttp.begin(dlSecureClient, url);
  } else {
    began = dlHttp.begin(dlPlainClient, url);
  }
  if (!began) {
    Serial.println("[ERROR] invalid url");
    return;
  }

  int code = dlHttp.GET();
  if (code <= 0 || code >= 400) {
    Serial.print("[ERROR] download request failed, code=");
    Serial.println(code);
    dlHttp.end();
    return;
  }

  dlTotalSize = dlHttp.getSize();
  dlBytesRead = 0;
  dlActive = true;

  Serial.print("[DOWNLOAD/START/SUCCESS]{\"size\":");
  Serial.print(dlTotalSize);
  Serial.println("}");
}

// Modeled directly on FlipperHTTP's HTTP::stream() (jblanked/FlipperHTTP,
// src/flipper-http/http.cpp): push raw bytes straight to Serial with no
// per-chunk framing, no ACKs, no baud switching. An idle-stall timeout on
// the HTTP stream is the only thing that can end the loop early. The
// receiver already knows the exact expected size (from
// DOWNLOAD/START/SUCCESS) so it reads exactly that many raw bytes back -
// no end-of-stream marker needs to survive being embedded in binary data.
void handleDownloadStream() {
  if (!dlActive) {
    Serial.println("[ERROR] no active download");
    return;
  }

  Serial.println("[DOWNLOAD/STREAM/BEGIN]");

  static uint8_t buf[DOWNLOAD_STREAM_FRAME];
  WiFiClient* stream = dlHttp.getStreamPtr();
  bool cancelled = false;
  bool errored = false;

  unsigned long timeoutStart = millis();
  const unsigned long timeoutInterval = HTTP_TIMEOUT_MS;

  while (dlHttp.connected() || stream->available() > 0) {
    if (dlTotalSize >= 0 && dlBytesRead >= dlTotalSize) break;

    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line == "[DOWNLOAD/CANCEL]") {
        cancelled = true;
        break;
      }
    }

    size_t avail = stream->available();
    if (avail > 0) {
      timeoutStart = millis();
      size_t want = avail > sizeof(buf) ? sizeof(buf) : avail;
      if (dlTotalSize >= 0) {
        int remaining = dlTotalSize - dlBytesRead;
        if ((int)want > remaining) want = remaining;
      }
      int got = stream->readBytes(buf, want);
      Serial.write(buf, got);
      dlBytesRead += got;
    } else {
      if (millis() - timeoutStart > timeoutInterval) {
        bool expectingMore = (dlTotalSize >= 0) && (dlBytesRead < dlTotalSize);
        if (expectingMore) errored = true;
        break;
      }
      delay(1);
    }
  }

  Serial.flush();
  Serial.println();

  if (cancelled) {
    Serial.println("[DOWNLOAD/CANCEL/SUCCESS]");
  } else if (errored) {
    Serial.println("[ERROR] stream incomplete");
  } else {
    Serial.println("[DOWNLOAD/STREAM/END]");
  }

  dlCleanup();
}

void handleDownloadCancel() {
  dlCleanup();
  Serial.println("[DOWNLOAD/CANCEL/SUCCESS]");
}

void handleBaudSet(const String& rest) {
  long rate = rest.toInt();
  if (rate < 9600) {
    Serial.println("[ERROR] invalid baud");
    return;
  }
  Serial.print("[BAUD/SET/SUCCESS]");
  Serial.println(rate);
  Serial.flush();
  delay(30);
  Serial.begin(rate);
}

bool githubGetJson(const String& url, String* outBody, int* outCode) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(secureClient, url)) return false;
  http.addHeader("User-Agent", "FoxESP32FW");
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  *outCode = code;
  if (code <= 0) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  ChunkedRead cr;
  chunkedReadInit(&cr, http);
  char buf[256];
  unsigned long start = millis();
  while ((http.connected() || stream->available()) &&
         (millis() - start) < (unsigned long)HTTP_TIMEOUT_MS) {
    int got = chunkedReadNext(&cr, stream, buf, sizeof(buf));
    if (got > 0) {
      outBody->concat(buf, got);
      start = millis();
    } else {
      if (cr.done) break;
      delay(1);
    }
  }
  http.end();
  return true;
}

bool githubGetJsonHead(const String& url, String* outBody, int* outCode) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(secureClient, url)) return false;
  http.addHeader("User-Agent", "FoxESP32FW");
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  *outCode = code;
  if (code <= 0) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  ChunkedRead cr;
  chunkedReadInit(&cr, http);
  char buf[256];
  unsigned long start = millis();
  bool haveTag = false;
  bool haveId = false;
  while ((http.connected() || stream->available()) &&
         (millis() - start) < (unsigned long)HTTP_TIMEOUT_MS) {
    int got = chunkedReadNext(&cr, stream, buf, sizeof(buf));
    if (got > 0) {
      outBody->concat(buf, got);
      start = millis();
      if (!haveTag) haveTag = outBody->indexOf("\"tag_name\"") >= 0;
      if (!haveId) haveId = outBody->indexOf("\"id\"") >= 0;
      if (haveTag && haveId) break;
    } else {
      if (cr.done) break;
      delay(1);
    }
  }
  http.end();
  return true;
}

#define ASSET_STREAM_MAX 32
#define ASSET_STREAM_HARD_CAP_MS 6000

bool githubStreamAssets(const String& url, int* outCode, int* outCount) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(secureClient, url)) return false;
  http.addHeader("User-Agent", "FoxESP32FW");
  http.addHeader("Accept", "application/vnd.github+json");
  int code = http.GET();
  *outCode = code;
  if (code <= 0) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  ChunkedRead cr;
  chunkedReadInit(&cr, http);
  long expectedSize = http.getSize();
  uint32_t totalBytesRead = 0;
  String buf;
  int depth = 0;
  bool inStr = false;
  int objStart = -1;
  int i = 0;
  char chunk[256];
  unsigned long start = millis();
  unsigned long hardDeadline = start + ASSET_STREAM_HARD_CAP_MS;

  String pendingNames[ASSET_STREAM_MAX];
  String pendingUrls[ASSET_STREAM_MAX];
  String pendingSizes[ASSET_STREAM_MAX];
  int pendingCount = 0;

  while ((http.connected() || stream->available()) &&
         (millis() - start) < (unsigned long)HTTP_TIMEOUT_MS &&
         millis() < hardDeadline) {
    int got = chunkedReadNext(&cr, stream, chunk, sizeof(chunk));
    if (got <= 0) {
      if (cr.done) break;
      delay(1);
      continue;
    }
    start = millis();
    totalBytesRead += (uint32_t)got;
    buf.concat(chunk, got);

    while (i < (int)buf.length()) {
      char c = buf[i];
      if (inStr) {
        if (c == '\\') {
          if (i + 1 < (int)buf.length()) { i += 2; continue; }
          break;
        }
        if (c == '"') inStr = false;
        i++;
        continue;
      }
      if (c == '"') { inStr = true; i++; continue; }
      if (c == '{') {
        if (depth == 0) objStart = i;
        depth++;
        i++;
        continue;
      }
      if (c == '}') {
        depth--;
        if (depth == 0 && objStart >= 0) {
          String elem = buf.substring(objStart, i + 1);
          String name, dlUrl, sizeStr;
          jsonExtractString(elem, "name", &name);
          jsonExtractString(elem, "browser_download_url", &dlUrl);
          jsonExtractString(elem, "size", &sizeStr);
          if (name.length() > 0 && pendingCount < ASSET_STREAM_MAX) {
            pendingNames[pendingCount] = name;
            pendingUrls[pendingCount] = dlUrl;
            pendingSizes[pendingCount] = sizeStr.length() ? sizeStr : "0";
            pendingCount++;
          }
          buf = buf.substring(i + 1);
          objStart = -1;
          i = 0;
          continue;
        }
        i++;
        continue;
      }
      i++;
    }
  }

  bool completeStream =
      expectedSize >= 0 ? (totalBytesRead >= (uint32_t)expectedSize) : cr.done;
  if (!completeStream) {
    http.end();
    return false;
  }

  if (outCount) *outCount = pendingCount;

  for (int a = 0; a < pendingCount; a++) {
    Serial.print("{\"name\":\"");
    Serial.print(pendingNames[a]);
    Serial.print("\",\"url\":\"");
    Serial.print(pendingUrls[a]);
    Serial.print("\",\"size\":");
    Serial.print(pendingSizes[a]);
    Serial.println("}");
  }

  http.end();
  return true;
}

void handleReleaseCheck(const String& json) {
  String repo;
  if (!jsonExtractString(json, "repo", &repo)) {
    Serial.println("[ERROR] missing repo");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] not connected to WiFi");
    return;
  }
  bool needCommit = json.indexOf("\"needCommit\":true") >= 0;
  bool needAssets = json.indexOf("\"needAssets\":false") < 0;

  String body;
  int code;
  String url = "https://api.github.com/repos/" + repo + "/releases/latest";
  if (!githubGetJsonHead(url, &body, &code)) {
    Serial.println("[ERROR] release lookup failed");
    return;
  }
  if (code == 404) {
    Serial.println("[ERROR] no releases found");
    return;
  }
  if (code != 200) {
    Serial.print("[ERROR] github api error, code=");
    Serial.println(code);
    return;
  }

  String tag;
  if (!jsonExtractString(body, "tag_name", &tag)) {
    Serial.println("[ERROR] No tag_name in release");
    return;
  }

  String releaseId;
  jsonExtractString(body, "id", &releaseId);
  body = "";

  String commit = "";
  if (needCommit) {
    delay(50);
    String refBody;
    int refCode;
    String refUrl = "https://api.github.com/repos/" + repo + "/git/refs/tags/" + tag;
    if (githubGetJson(refUrl, &refBody, &refCode) && refCode == 200) {
      String objStr;
      if (extractBalanced(refBody, "object", '{', '}', &objStr)) {
        String sha, type;
        jsonExtractString(objStr, "sha", &sha);
        jsonExtractString(objStr, "type", &type);
        refBody = "";
        if (type == "tag") {
          delay(50);
          String tagBody;
          int tagCode;
          String tagUrl = "https://api.github.com/repos/" + repo + "/git/tags/" + sha;
          if (githubGetJson(tagUrl, &tagBody, &tagCode) && tagCode == 200) {
            String innerObj;
            if (extractBalanced(tagBody, "object", '{', '}', &innerObj)) {
              jsonExtractString(innerObj, "sha", &commit);
            }
          }
        } else {
          commit = sha;
        }
      }
    }
  }
  if (commit.length() > 8) commit = commit.substring(0, 8);

  Serial.print("[RELEASE/CHECK/SUCCESS]{\"tag\":\"");
  Serial.print(tag);
  Serial.print("\",\"commit\":\"");
  Serial.print(commit);
  Serial.println("\"}");

  if (!needAssets) {
    Serial.println("[RELEASE/CHECK/END]");
    return;
  }

  if (releaseId.length() > 0) {
    String assetsBaseUrl =
        "https://api.github.com/repos/" + repo + "/releases/" + releaseId + "/assets";
    const int assetsPerPage = 6;
    int page = 1;
    bool assetsOk = true;
    int assetsCode = 0;
    while (true) {
      String pageUrl =
          assetsBaseUrl + "?per_page=" + String(assetsPerPage) + "&page=" + String(page);
      bool pageOk = false;
      int pageCount = 0;
      for (int attempt = 0; attempt < 2 && !pageOk; attempt++) {
        delay(attempt == 0 ? 50 : 300);
        assetsCode = 0;
        pageCount = 0;
        pageOk = githubStreamAssets(pageUrl, &assetsCode, &pageCount) && assetsCode == 200;
      }
      if (!pageOk) {
        assetsOk = false;
        break;
      }
      if (pageCount == 0) break;
      page++;
      if (page > 20) break;
    }
    if (!assetsOk) {
      Serial.print("[ERROR] assets fetch failed, code=");
      Serial.println(assetsCode);
    }
  } else {
    Serial.println("[ERROR] no releaseId, skipping assets fetch");
  }

  Serial.println("[RELEASE/CHECK/END]");
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
      "[WIFI/LIST]", "[WIFI/SAVED/LIST]", "[WIFI/IP]", "[WIFI/SSID]", "[WIFI/STATUS]",
      "[IP/ADDRESS]", "[WIFI/AP]",
      "[GET]", "[GET/HTTP]", "[POST/HTTP]", "[PUT/HTTP]", "[PATCH/HTTP]", "[DELETE/HTTP]",
      "[GET/BYTES]", "[POST/BYTES]",
      "[SOCKET/START]", "[SOCKET/SEND]", "[SOCKET/STOP]",
      "[PARSE]", "[PARSE/ARRAY]", "[LED/ON]", "[LED/OFF]",
      "[RELEASE/CHECK]", "[DOWNLOAD/START]", "[DOWNLOAD/STREAM]", "[DOWNLOAD/CANCEL]",
      "[BAUD/SET]"
    };
    for (auto c : cmds) Serial.println(c);

    Serial.println(
      "[LIST], [PING], [REBOOT], [WIFI/IP], [WIFI/SCAN], [WIFI/SAVE], [WIFI/CONNECT], "
      "[WIFI/DISCONNECT], [WIFI/LIST], [GET], [GET/HTTP], [POST/HTTP], [PUT/HTTP], "
      "[DELETE/HTTP], [GET/BYTES], [POST/BYTES], [PARSE], [PARSE/ARRAY], [LED/ON], "
      "[LED/OFF], [IP/ADDRESS], [WIFI/AP], [RELEASE/CHECK], [DOWNLOAD/START], "
      "[DOWNLOAD/STREAM], [DOWNLOAD/CANCEL], [BAUD/SET]");
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
  if (cmd == "WIFI/DISCONNECT") {
    WiFi.disconnect();
    Serial.println("[DISCONNECTED] WiFi has been disconnected.");
    return true;
  }
  if (cmd == "WIFI/FORGET") { handleWifiForget(rest); return true; }
  if (cmd == "WIFI/LIST") { handleWifiList(); return true; }
  if (cmd == "WIFI/SAVED/LIST") { handleWifiSavedList(); return true; }

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
    handleWifiIp();
    return true;
  }

  if (cmd == "IP/ADDRESS") {
    Serial.println(WiFi.localIP().toString());
    return true;
  }

  if (cmd == "WIFI/AP") { handleWifiAp(rest); return true; }

  if (cmd == "GET") { handlePlainGet(rest); return true; }
  if (cmd == "GET/BYTES") { handleGetBytes(rest); return true; }
  if (cmd == "POST/BYTES") { handlePostBytes(rest); return true; }

  if (cmd == "GET/HTTP") {
    String url; jsonExtractString(rest, "url", &url);
    doHttpRequest("GET", url, "", rest, "[GET/SUCCESS]");
    return true;
  }
  if (cmd == "POST/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("POST", url, payload, rest, "[POST/SUCCESS]");
    return true;
  }
  if (cmd == "PUT/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("PUT", url, payload, rest, "[PUT/SUCCESS]");
    return true;
  }
  if (cmd == "PATCH/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("PATCH", url, payload, rest, "[PATCH/SUCCESS]");
    return true;
  }
  if (cmd == "DELETE/HTTP") {
    String url, payload; jsonExtractString(rest, "url", &url); jsonExtractString(rest, "payload", &payload);
    doHttpRequest("DELETE", url, payload, rest, "[DELETE/SUCCESS]");
    return true;
  }

  if (cmd == "PARSE") { handleParse(rest); return true; }
  if (cmd == "PARSE/ARRAY") { handleParseArray(rest); return true; }

  if (cmd == "RELEASE/CHECK") { handleReleaseCheck(rest); return true; }
  if (cmd == "DOWNLOAD/START") { handleDownloadStart(rest); return true; }
  if (cmd == "DOWNLOAD/STREAM") { handleDownloadStream(); return true; }
  if (cmd == "DOWNLOAD/CANCEL") { handleDownloadCancel(); return true; }
  if (cmd == "BAUD/SET") { handleBaudSet(rest); return true; }

  if (cmd == "SOCKET/START") { handleSocketStart(rest); return true; }
  if (cmd == "SOCKET/STOP") { handleSocketStop(); return true; }
  if (cmd == "SOCKET/SEND") { handleSocketSend(rest); return true; }

  if (cmd == "LED/ON") { digitalWrite(STATUS_LED_PIN, HIGH); Serial.println("[LED/ON/SUCCESS]"); return true; }
  if (cmd == "LED/OFF") { digitalWrite(STATUS_LED_PIN, LOW); Serial.println("[LED/OFF/SUCCESS]"); return true; }

  Serial.println("[ERROR] unknown command");
  return true;
}
}
