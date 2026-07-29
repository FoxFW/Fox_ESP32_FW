#include "script_engine.h"
#include "config.h"
#include "ble_bridge.h"
#include "ble_attack.h"
#include "wifi_attack.h"
#include "wifi_recon.h"
#include "settings.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>
#include <ctype.h>
#include <string.h>

namespace {
enum class VT { UNDEF, NUM, STR, BOOL, ARRAY, OBJECT };

struct Value {
  VT type = VT::UNDEF;
  double num = 0;
  String str;
  bool b = false;
  int ref = -1;

  static Value ofNum(double n) { Value v; v.type = VT::NUM; v.num = n; return v; }
  static Value ofStr(const String& s) { Value v; v.type = VT::STR; v.str = s; return v; }
  static Value ofBool(bool bb) { Value v; v.type = VT::BOOL; v.b = bb; return v; }
  static Value ofArray(int slot) { Value v; v.type = VT::ARRAY; v.ref = slot; return v; }
  static Value ofObject(int slot) { Value v; v.type = VT::OBJECT; v.ref = slot; return v; }

  double asNum() const {
    if (type == VT::NUM) return num;
    if (type == VT::BOOL) return b ? 1 : 0;
    if (type == VT::STR) return str.toFloat();
    return 0;
  }
  bool asBool() const {
    if (type == VT::BOOL) return b;
    if (type == VT::NUM) return num != 0;
    if (type == VT::STR) return str.length() > 0;
    if (type == VT::ARRAY || type == VT::OBJECT) return ref >= 0;
    return false;
  }
  String asStr() const {
    if (type == VT::STR) return str;
    if (type == VT::NUM) {
      if (num == (long)num) return String((long)num);
      return String(num);
    }
    if (type == VT::BOOL) return b ? "true" : "false";
    if (type == VT::ARRAY) return "[array]";
    if (type == VT::OBJECT) return "[object]";
    return "undefined";
  }
};

enum class TT { NUM, STR, IDENT, PUNCT, END };

struct Token {
  TT type = TT::END;
  String text;
  double num = 0;
};

bool isIdentStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
bool isIdentChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

int tokenize(const String& src, Token* out, int maxTokens) {
  int n = 0;
  size_t i = 0;
  size_t len = src.length();

  while (i < len) {
    char c = src[i];

    if (isspace((unsigned char)c)) { i++; continue; }

    if (c == '/' && i + 1 < len && src[i + 1] == '/') {
      while (i < len && src[i] != '\n') i++;
      continue;
    }

    if (n >= maxTokens - 1) return -1;

    if (isdigit((unsigned char)c) || (c == '.' && i + 1 < len && isdigit((unsigned char)src[i + 1]))) {
      size_t start = i;
      while (i < len && (isdigit((unsigned char)src[i]) || src[i] == '.')) i++;
      out[n].type = TT::NUM;
      out[n].num = src.substring(start, i).toFloat();
      n++;
      continue;
    }

    if (c == '"') {
      i++;
      String s;
      while (i < len && src[i] != '"') {
        if (src[i] == '\\' && i + 1 < len) {
          char esc = src[i + 1];
          if (esc == 'n') s += '\n';
          else if (esc == 't') s += '\t';
          else s += esc;
          i += 2;
        } else {
          s += src[i];
          i++;
        }
        if (s.length() > SCRIPT_STRING_MAX) break;
      }
      if (i < len && src[i] == '"') i++;
      out[n].type = TT::STR;
      out[n].text = s;
      n++;
      continue;
    }

    if (isIdentStart(c)) {
      size_t start = i;
      while (i < len && isIdentChar(src[i])) i++;
      out[n].type = TT::IDENT;
      out[n].text = src.substring(start, i);
      n++;
      continue;
    }

    if (i + 1 < len) {
      String two = src.substring(i, i + 2);
      if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||") {
        out[n].type = TT::PUNCT;
        out[n].text = two;
        n++;
        i += 2;
        continue;
      }
    }

    static const char singles[] = "(){}[];,.:=+-*/%<>!";
    if (strchr(singles, c) != nullptr) {
      out[n].type = TT::PUNCT;
      out[n].text = String(c);
      n++;
      i++;
      continue;
    }

    i++;
  }

  out[n].type = TT::END;
  out[n].text = "";
  n++;
  return n;
}

Preferences scriptStorage;

bool nativeWifiConnect(const String& ssid, const String& pass) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000UL) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

String nativeWifiIp() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
}

String nativeHttpGet(const String& url) {
  if (WiFi.status() != WL_CONNECTED) return "";
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
  if (!began) return "";
  int code = http.GET();
  String body;
  if (code > 0) {
    body = http.getString();
    if ((int)body.length() > SCRIPT_HTTP_GET_MAX) body = body.substring(0, SCRIPT_HTTP_GET_MAX);
  }
  http.end();
  return body;
}

String nativeHttpPost(const String& url, const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return "";
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
  if (!began) return "";
  int code = http.POST(payload);
  String body;
  if (code > 0) {
    body = http.getString();
    if ((int)body.length() > SCRIPT_HTTP_GET_MAX) body = body.substring(0, SCRIPT_HTTP_GET_MAX);
  }
  http.end();
  return body;
}

bool nativeGpioValid(int pin) { return pin >= 0 && pin <= 39; }

bool nativeGpioMode(int pin, const String& mode) {
  if (!nativeGpioValid(pin)) return false;
  String m = mode;
  m.toUpperCase();
  if (m == "OUT" || m == "OUTPUT") { pinMode(pin, OUTPUT); return true; }
  if (m == "IN" || m == "INPUT") { pinMode(pin, INPUT); return true; }
  if (m == "PULLUP" || m == "INPUT_PULLUP") { pinMode(pin, INPUT_PULLUP); return true; }
  return false;
}

bool nativeGpioWrite(int pin, int value) {
  if (!nativeGpioValid(pin)) return false;
  digitalWrite(pin, value != 0 ? HIGH : LOW);
  return true;
}

int nativeGpioRead(int pin) {
  if (!nativeGpioValid(pin)) return -1;
  return digitalRead(pin);
}

String nativeStorageGet(const String& key) {
  if (key.length() == 0 || key.length() > SCRIPT_STORAGE_KEY_MAX) return "";
  scriptStorage.begin("foxscript", true);
  String v = scriptStorage.getString(key.c_str(), "");
  scriptStorage.end();
  return v;
}

bool nativeStorageSet(const String& key, const String& value) {
  if (key.length() == 0 || key.length() > SCRIPT_STORAGE_KEY_MAX) return false;
  if ((int)value.length() > SCRIPT_STORAGE_VALUE_MAX) return false;
  scriptStorage.begin("foxscript", false);
  bool ok = scriptStorage.putString(key.c_str(), value) > 0 || value.length() == 0;
  scriptStorage.end();
  return ok;
}

struct ArraySlot {
  bool used = false;
  Value items[SCRIPT_ARRAY_LEN_MAX];
  int count = 0;
};

struct ObjectSlot {
  bool used = false;
  String keys[SCRIPT_OBJECT_KEYS_MAX];
  Value vals[SCRIPT_OBJECT_KEYS_MAX];
  int count = 0;
};

ArraySlot g_arrays[SCRIPT_ARRAYS_MAX];
ObjectSlot g_objects[SCRIPT_OBJECTS_MAX];

void resetArraysAndObjects() {
  for (int i = 0; i < SCRIPT_ARRAYS_MAX; i++) { g_arrays[i].used = false; g_arrays[i].count = 0; }
  for (int i = 0; i < SCRIPT_OBJECTS_MAX; i++) { g_objects[i].used = false; g_objects[i].count = 0; }
}

int allocArray() {
  for (int i = 0; i < SCRIPT_ARRAYS_MAX; i++) {
    if (!g_arrays[i].used) { g_arrays[i].used = true; g_arrays[i].count = 0; return i; }
  }
  return -1;
}
int allocObject() {
  for (int i = 0; i < SCRIPT_OBJECTS_MAX; i++) {
    if (!g_objects[i].used) { g_objects[i].used = true; g_objects[i].count = 0; return i; }
  }
  return -1;
}

bool arrayPush(int slot, const Value& v) {
  if (slot < 0 || slot >= SCRIPT_ARRAYS_MAX || !g_arrays[slot].used) return false;
  ArraySlot& a = g_arrays[slot];
  if (a.count >= SCRIPT_ARRAY_LEN_MAX) return false;
  a.items[a.count++] = v;
  return true;
}
Value arrayGet(int slot, int idx) {
  if (slot < 0 || slot >= SCRIPT_ARRAYS_MAX || !g_arrays[slot].used) return Value();
  ArraySlot& a = g_arrays[slot];
  if (idx < 0 || idx >= a.count) return Value();
  return a.items[idx];
}
bool arraySet(int slot, int idx, const Value& v) {
  if (slot < 0 || slot >= SCRIPT_ARRAYS_MAX || !g_arrays[slot].used) return false;
  ArraySlot& a = g_arrays[slot];

  if (idx == a.count) return arrayPush(slot, v);
  if (idx < 0 || idx >= a.count) return false;
  a.items[idx] = v;
  return true;
}
int arrayLen(int slot) {
  if (slot < 0 || slot >= SCRIPT_ARRAYS_MAX || !g_arrays[slot].used) return 0;
  return g_arrays[slot].count;
}
Value arrayPop(int slot) {
  if (slot < 0 || slot >= SCRIPT_ARRAYS_MAX || !g_arrays[slot].used) return Value();
  ArraySlot& a = g_arrays[slot];
  if (a.count == 0) return Value();
  return a.items[--a.count];
}

int objectFindKey(int slot, const String& key) {
  if (slot < 0 || slot >= SCRIPT_OBJECTS_MAX || !g_objects[slot].used) return -1;
  ObjectSlot& o = g_objects[slot];
  for (int i = 0; i < o.count; i++) if (o.keys[i] == key) return i;
  return -1;
}
Value objectGet(int slot, const String& key) {
  int i = objectFindKey(slot, key);
  if (i < 0) return Value();
  return g_objects[slot].vals[i];
}
bool objectSet(int slot, const String& key, const Value& v) {
  if (slot < 0 || slot >= SCRIPT_OBJECTS_MAX || !g_objects[slot].used) return false;
  ObjectSlot& o = g_objects[slot];
  int i = objectFindKey(slot, key);
  if (i >= 0) { o.vals[i] = v; return true; }
  if (o.count >= SCRIPT_OBJECT_KEYS_MAX) return false;
  o.keys[o.count] = key;
  o.vals[o.count] = v;
  o.count++;
  return true;
}
bool objectHas(int slot, const String& key) { return objectFindKey(slot, key) >= 0; }
int objectKeysArray(int slot) {
  int arr = allocArray();
  if (arr < 0 || slot < 0 || slot >= SCRIPT_OBJECTS_MAX || !g_objects[slot].used) return arr;
  for (int i = 0; i < g_objects[slot].count; i++) arrayPush(arr, Value::ofStr(g_objects[slot].keys[i]));
  return arr;
}

struct FuncDef {
  bool used = false;
  String name;
  String params[SCRIPT_FUNC_PARAMS_MAX];
  int paramCount = 0;
  int bodyPos = 0;
};

class Interp {
public:
  Token* toks = nullptr;
  int count = 0;
  int pos = 0;

  String varNames[SCRIPT_CALL_DEPTH_MAX + 1][SCRIPT_VARS_MAX];
  Value varValues[SCRIPT_CALL_DEPTH_MAX + 1][SCRIPT_VARS_MAX];
  int varCount[SCRIPT_CALL_DEPTH_MAX + 1] = {0};
  int frameDepth = 0;

  FuncDef funcs[SCRIPT_FUNCS_MAX];
  int funcCount = 0;

  bool returning = false;
  Value returnValue;

  bool error = false;
  String errMsg;
  int stepBudget = 0;

  Token& cur() { return toks[pos]; }
  bool isEnd() { return pos >= count || toks[pos].type == TT::END; }

  bool matchPunct(const char* p) {
    if (!isEnd() && cur().type == TT::PUNCT && cur().text == p) { pos++; return true; }
    return false;
  }
  bool expectPunct(const char* p) {
    if (matchPunct(p)) return true;
    fail(String("expected '") + p + "'");
    return false;
  }
  void fail(const String& m) {
    if (!error) { error = true; errMsg = m; }
  }
  bool budgetExceeded() {
    if (--stepBudget <= 0) { fail("script exceeded step budget"); return true; }
    return false;
  }

  int findVar(const String& name) {
    for (int i = 0; i < varCount[frameDepth]; i++) if (varNames[frameDepth][i] == name) return i;
    return -1;
  }
  void setVar(const String& name, const Value& v) {
    int idx = findVar(name);
    if (idx >= 0) { varValues[frameDepth][idx] = v; return; }
    if (varCount[frameDepth] >= SCRIPT_VARS_MAX) { fail("too many variables"); return; }
    varNames[frameDepth][varCount[frameDepth]] = name;
    varValues[frameDepth][varCount[frameDepth]] = v;
    varCount[frameDepth]++;
  }
  Value getVar(const String& name) {
    int idx = findVar(name);
    return idx >= 0 ? varValues[frameDepth][idx] : Value();
  }

  int findFunc(const String& name) {
    for (int i = 0; i < funcCount; i++) if (funcs[i].used && funcs[i].name == name) return i;
    return -1;
  }

  Value invoke(const String& name, Value* args, int argc, bool exec) {
    int dot = name.indexOf('.');
    if (dot < 0) {
      int fi = findFunc(name);
      if (fi >= 0) return callUserFunction(fi, args, argc, exec);
    } else if (name.indexOf('.', dot + 1) < 0) {
      String base = name.substring(0, dot);
      String method = name.substring(dot + 1);
      Value baseVal = exec ? getVar(base) : Value();
      if (exec && baseVal.type == VT::ARRAY) return callArrayMethod(baseVal.ref, method, args, argc);
      if (exec && baseVal.type == VT::OBJECT) return callObjectMethod(baseVal.ref, method, args, argc);
    }
    return callNative(name, args, argc, exec);
  }

  Value callArrayMethod(int slot, const String& method, Value* args, int argc) {
    if (method == "push") {
      if (argc < 1) { fail("array.push(value) needs a value"); return Value(); }
      if (!arrayPush(slot, args[0])) fail("array is full");
      return Value();
    }
    if (method == "len") return Value::ofNum(arrayLen(slot));
    if (method == "pop") return arrayPop(slot);
    fail("unknown array method: " + method);
    return Value();
  }
  Value callObjectMethod(int slot, const String& method, Value* args, int argc) {
    if (method == "has") {
      if (argc < 1) { fail("object.has(key) needs a key"); return Value(); }
      return Value::ofBool(objectHas(slot, args[0].asStr()));
    }
    if (method == "keys") return Value::ofArray(objectKeysArray(slot));
    fail("unknown object method: " + method);
    return Value();
  }

  Value callUserFunction(int fi, Value* args, int argc, bool exec) {
    if (frameDepth >= SCRIPT_CALL_DEPTH_MAX) {
      fail("call stack too deep (max " + String(SCRIPT_CALL_DEPTH_MAX) + ")");
      return Value();
    }
    FuncDef& fn = funcs[fi];
    int callerPos = pos;

    frameDepth++;
    varCount[frameDepth] = 0;
    for (int i = 0; i < fn.paramCount; i++) {
      setVar(fn.params[i], (i < argc) ? args[i] : Value());
    }

    pos = fn.bodyPos;
    expectPunct("{");
    while (!error && !returning && !isEnd() && !(cur().type == TT::PUNCT && cur().text == "}")) {
      execStatement(exec);
    }
    matchPunct("}");

    Value result = returning ? returnValue : Value();
    returning = false;
    returnValue = Value();
    frameDepth--;
    pos = callerPos;
    return result;
  }

  Value callNative(const String& name, Value* args, int argc, bool exec) {
    if (name == "print") {
      if (exec) {
        for (int i = 0; i < argc; i++) {
          if (i > 0) Serial.print(" ");
          Serial.print(args[i].asStr());
        }
        Serial.println();
      }
      return Value();
    }
    if (name == "delay") {
      if (exec && argc >= 1) delay((unsigned long)max(0.0, min(args[0].asNum(), 20000.0)));
      return Value();
    }

    if (name == "wifi.connect") {
      if (argc < 1) { fail("wifi.connect(ssid, password) needs at least ssid"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(nativeWifiConnect(args[0].asStr(), argc >= 2 ? args[1].asStr() : ""));
    }
    if (name == "wifi.disconnect") {
      if (exec) WiFi.disconnect();
      return Value();
    }
    if (name == "wifi.ip") return Value::ofStr(exec ? nativeWifiIp() : "");
    if (name == "wifi.ssid") return Value::ofStr(exec && WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "");
    if (name == "wifi.status") return Value::ofBool(exec && WiFi.status() == WL_CONNECTED);
    if (name == "wifi.scan") return Value::ofNum(exec ? (double)FoxWifiRecon::scriptScanApCount() : 0);

    if (name == "http.get") {
      if (argc < 1) { fail("http.get(url) needs a url"); return Value(); }
      if (!exec) return Value::ofStr("");
      return Value::ofStr(nativeHttpGet(args[0].asStr()));
    }
    if (name == "http.post") {
      if (argc < 1) { fail("http.post(url, body) needs a url"); return Value(); }
      if (!exec) return Value::ofStr("");
      return Value::ofStr(nativeHttpPost(args[0].asStr(), argc >= 2 ? args[1].asStr() : ""));
    }

    if (name == "ble.write") {
      if (argc < 1) { fail("ble.write(hex) needs a hex string"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(FoxBle::writeHex(args[0].asStr()));
    }
    if (name == "ble.connected") return Value::ofBool(exec && FoxBle::isConnected());
    if (name == "ble.scan") {
      if (exec) FoxBle::scriptScan();
      return Value();
    }
    if (name == "ble.spam") {
      if (argc < 1) { fail("ble.spam(mode) needs a mode string"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(FoxBleAttack::scriptSpam(args[0].asStr()));
    }

    if (name == "wifiattack.deauth") return Value::ofBool(exec && FoxWifiAttack::scriptDeauth());
    if (name == "wifiattack.beacon") {
      if (argc < 1) { fail("wifiattack.beacon(ssid) needs an ssid"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(FoxWifiAttack::scriptBeaconSpam(args[0].asStr()));
    }
    if (name == "wifiattack.portscan") {
      if (argc < 3) { fail("wifiattack.portscan(ip, startPort, endPort) needs 3 args"); return Value(); }
      if (!exec) return Value::ofNum(-1);
      return Value::ofNum(FoxWifiAttack::scriptPortScan(args[0].asStr(), (int)args[1].asNum(), (int)args[2].asNum()));
    }

    if (name == "gpio.mode") {
      if (argc < 2) { fail("gpio.mode(pin, mode) needs a pin and mode"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(nativeGpioMode((int)args[0].asNum(), args[1].asStr()));
    }
    if (name == "gpio.write") {
      if (argc < 2) { fail("gpio.write(pin, value) needs a pin and value"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(nativeGpioWrite((int)args[0].asNum(), (int)args[1].asNum()));
    }
    if (name == "gpio.read") {
      if (argc < 1) { fail("gpio.read(pin) needs a pin"); return Value(); }
      if (!exec) return Value::ofNum(-1);
      return Value::ofNum(nativeGpioRead((int)args[0].asNum()));
    }

    if (name == "led.on") {
      if (exec) { pinMode(STATUS_LED_PIN, OUTPUT); digitalWrite(STATUS_LED_PIN, HIGH); }
      return Value();
    }
    if (name == "led.off") {
      if (exec) { pinMode(STATUS_LED_PIN, OUTPUT); digitalWrite(STATUS_LED_PIN, LOW); }
      return Value();
    }

    if (name == "storage.set") {
      if (argc < 2) { fail("storage.set(key, value) needs a key and value"); return Value(); }
      if (!exec) return Value::ofBool(false);
      return Value::ofBool(nativeStorageSet(args[0].asStr(), args[1].asStr()));
    }
    if (name == "storage.get") {
      if (argc < 1) { fail("storage.get(key) needs a key"); return Value(); }
      if (!exec) return Value::ofStr("");
      return Value::ofStr(nativeStorageGet(args[0].asStr()));
    }

    if (name == "device.name") return Value::ofStr("FoxESP32");
    if (name == "device.freeHeap") return Value::ofNum(exec ? (double)ESP.getFreeHeap() : 0);
    if (name == "device.uptime") return Value::ofNum(exec ? (double)(millis() / 1000) : 0);
    if (name == "device.reboot") {
      if (exec) { Serial.println("REBOOTING"); delay(100); ESP.restart(); }
      return Value();
    }

    if (name == "settings.attacksEnabled") return Value::ofBool(exec && FoxSettings::attacksEnabled());

    if (name == "rfid.scan" || name == "rfid.read" || name == "rfid.write") {
      return Value::ofStr(FOX_HAS_RFID ? "NOTIMPLEMENTED" : "NORFID");
    }
    if (name == "subghz.scan" || name == "subghz.tx") {
      return Value::ofStr(FOX_HAS_SUBGHZ ? "NOTIMPLEMENTED" : "NOSUBGHZ");
    }
    if (name == "ir.send") {
      return Value::ofStr(FOX_HAS_IR ? "NOTIMPLEMENTED" : "NOIR");
    }
    if (name == "gps.fix" || name == "gps.lat" || name == "gps.lon") {
      return Value::ofStr(FOX_HAS_GPS ? "NOTIMPLEMENTED" : "NOGPS");
    }

    fail("unknown function: " + name);
    return Value();
  }

  Value parseOr(bool exec) {
    if (budgetExceeded()) return Value();
    Value l = parseAnd(exec);
    while (!error && !isEnd() && cur().type == TT::PUNCT && cur().text == "||") {
      pos++;
      Value r = parseAnd(exec);
      if (exec) l = Value::ofBool(l.asBool() || r.asBool());
    }
    return l;
  }
  Value parseAnd(bool exec) {
    Value l = parseEquality(exec);
    while (!error && !isEnd() && cur().type == TT::PUNCT && cur().text == "&&") {
      pos++;
      Value r = parseEquality(exec);
      if (exec) l = Value::ofBool(l.asBool() && r.asBool());
    }
    return l;
  }
  Value parseEquality(bool exec) {
    Value l = parseRelational(exec);
    while (!error && !isEnd() && cur().type == TT::PUNCT && (cur().text == "==" || cur().text == "!=")) {
      String op = cur().text; pos++;
      Value r = parseRelational(exec);
      if (exec) {
        bool eq = (l.type == VT::STR || r.type == VT::STR) ? l.asStr() == r.asStr() : l.asNum() == r.asNum();
        l = Value::ofBool(op == "==" ? eq : !eq);
      }
    }
    return l;
  }
  Value parseRelational(bool exec) {
    Value l = parseAdditive(exec);
    while (!error && !isEnd() && cur().type == TT::PUNCT &&
           (cur().text == "<" || cur().text == "<=" || cur().text == ">" || cur().text == ">=")) {
      String op = cur().text; pos++;
      Value r = parseAdditive(exec);
      if (exec) {
        double a = l.asNum(), b = r.asNum();
        bool res = (op == "<") ? a < b : (op == "<=") ? a <= b : (op == ">") ? a > b : a >= b;
        l = Value::ofBool(res);
      }
    }
    return l;
  }
  Value parseAdditive(bool exec) {
    Value l = parseMultiplicative(exec);
    while (!error && !isEnd() && cur().type == TT::PUNCT && (cur().text == "+" || cur().text == "-")) {
      String op = cur().text; pos++;
      Value r = parseMultiplicative(exec);
      if (exec) {
        if (op == "+") {
          l = (l.type == VT::STR || r.type == VT::STR) ? Value::ofStr(l.asStr() + r.asStr())
                                                         : Value::ofNum(l.asNum() + r.asNum());
        } else {
          l = Value::ofNum(l.asNum() - r.asNum());
        }
      }
    }
    return l;
  }
  Value parseMultiplicative(bool exec) {
    Value l = parseUnary(exec);
    while (!error && !isEnd() && cur().type == TT::PUNCT &&
           (cur().text == "*" || cur().text == "/" || cur().text == "%")) {
      String op = cur().text; pos++;
      Value r = parseUnary(exec);
      if (exec) {
        double a = l.asNum(), b = r.asNum();
        double res = (op == "*") ? a * b : (op == "/") ? (b != 0 ? a / b : 0) : (b != 0 ? fmod(a, b) : 0);
        l = Value::ofNum(res);
      }
    }
    return l;
  }
  Value parseUnary(bool exec) {
    if (!isEnd() && cur().type == TT::PUNCT && cur().text == "!") {
      pos++;
      Value v = parseUnary(exec);
      return exec ? Value::ofBool(!v.asBool()) : Value();
    }
    if (!isEnd() && cur().type == TT::PUNCT && cur().text == "-") {
      pos++;
      Value v = parseUnary(exec);
      return exec ? Value::ofNum(-v.asNum()) : Value();
    }
    return parsePrimary(exec);
  }
  Value parsePrimary(bool exec) {
    if (isEnd()) { fail("unexpected end of script"); return Value(); }
    Token t = cur();

    if (t.type == TT::NUM) { pos++; return Value::ofNum(t.num); }
    if (t.type == TT::STR) { pos++; return Value::ofStr(t.text); }

    if (t.type == TT::PUNCT && t.text == "(") {
      pos++;
      Value v = parseOr(exec);
      expectPunct(")");
      return v;
    }

    if (t.type == TT::PUNCT && t.text == "[") {
      pos++;
      int slot = exec ? allocArray() : -1;
      if (exec && slot < 0) { fail("too many arrays (max " + String(SCRIPT_ARRAYS_MAX) + ")"); return Value(); }
      if (!(cur().type == TT::PUNCT && cur().text == "]")) {
        while (true) {
          Value v = parseOr(exec);
          if (exec && !arrayPush(slot, v)) { fail("array literal too long (max " + String(SCRIPT_ARRAY_LEN_MAX) + ")"); break; }
          if (matchPunct(",")) continue;
          break;
        }
      }
      expectPunct("]");
      return exec ? Value::ofArray(slot) : Value();
    }

    if (t.type == TT::PUNCT && t.text == "{") {
      pos++;
      int slot = exec ? allocObject() : -1;
      if (exec && slot < 0) { fail("too many objects (max " + String(SCRIPT_OBJECTS_MAX) + ")"); return Value(); }
      if (!(cur().type == TT::PUNCT && cur().text == "}")) {
        while (true) {
          if (isEnd() || cur().type != TT::STR) { fail("expected a quoted string key in object literal"); break; }
          String key = cur().text; pos++;
          expectPunct(":");
          Value v = parseOr(exec);
          if (exec && !objectSet(slot, key, v)) { fail("object literal has too many keys (max " + String(SCRIPT_OBJECT_KEYS_MAX) + ")"); break; }
          if (matchPunct(",")) continue;
          break;
        }
      }
      expectPunct("}");
      return exec ? Value::ofObject(slot) : Value();
    }

    if (t.type == TT::IDENT) {
      pos++;
      String name = t.text;
      while (!isEnd() && cur().type == TT::PUNCT && cur().text == ".") {
        pos++;
        if (isEnd() || cur().type != TT::IDENT) { fail("expected identifier after '.'"); return Value(); }
        name += ".";
        name += cur().text;
        pos++;
      }
      if (!isEnd() && cur().type == TT::PUNCT && cur().text == "(") {
        pos++;
        Value args[SCRIPT_CALL_ARGS_MAX];
        int argc = 0;
        if (!(cur().type == TT::PUNCT && cur().text == ")")) {
          while (true) {
            if (argc >= SCRIPT_CALL_ARGS_MAX) { fail("too many arguments"); break; }
            args[argc++] = parseOr(exec);
            if (matchPunct(",")) continue;
            break;
          }
        }
        expectPunct(")");
        return invoke(name, args, argc, exec);
      }

      Value result;
      int dot = name.indexOf('.');
      if (dot >= 0 && name.indexOf('.', dot + 1) < 0) {
        String base = name.substring(0, dot);
        String field = name.substring(dot + 1);
        Value baseVal = exec ? getVar(base) : Value();
        result = (exec && baseVal.type == VT::OBJECT) ? objectGet(baseVal.ref, field) : (exec ? getVar(name) : Value());
      } else {
        result = exec ? getVar(name) : Value();
      }

      if (!isEnd() && cur().type == TT::PUNCT && cur().text == "[") {
        pos++;
        Value idx = parseOr(exec);
        expectPunct("]");
        if (exec) {
          if (result.type == VT::ARRAY) result = arrayGet(result.ref, (int)idx.asNum());
          else if (result.type == VT::OBJECT) result = objectGet(result.ref, idx.asStr());
          else result = Value();
        }
      }
      return result;
    }

    fail("unexpected token near '" + t.text + "'");
    return Value();
  }

  void execBlock(bool exec) {
    expectPunct("{");
    while (!error && !isEnd() && !(cur().type == TT::PUNCT && cur().text == "}")) {
      execStatement(exec);
    }
    expectPunct("}");
  }

  int scanToCloseParen(int from) {
    int depth = 0;
    for (int i = from; i < count; i++) {
      if (toks[i].type == TT::PUNCT) {
        if (toks[i].text == "(") depth++;
        else if (toks[i].text == ")") {
          if (depth == 0) return i;
          depth--;
        }
      }
    }
    return count;
  }

  int scanToSemicolon(int from) {
    int depth = 0;
    for (int i = from; i < count; i++) {
      if (toks[i].type == TT::PUNCT) {
        if (toks[i].text == "(") depth++;
        else if (toks[i].text == ")") { if (depth > 0) depth--; }
        else if (toks[i].text == ";" && depth == 0) return i;
      }
    }
    return count;
  }

  void execStatement(bool exec) {
    if (error || isEnd()) return;
    if (budgetExceeded()) return;

    if (cur().type == TT::IDENT && cur().text == "let") {
      pos++;
      if (isEnd() || cur().type != TT::IDENT) { fail("expected variable name after let"); return; }
      String name = cur().text; pos++;
      expectPunct("=");
      Value v = parseOr(exec);
      matchPunct(";");
      if (exec) setVar(name, v);
      return;
    }

    if (cur().type == TT::IDENT && cur().text == "if") {
      pos++;
      expectPunct("(");
      Value cond = parseOr(exec);
      expectPunct(")");
      bool takeIf = exec && cond.asBool();
      execBlock(exec && takeIf);
      if (!error && !isEnd() && cur().type == TT::IDENT && cur().text == "else") {
        pos++;
        execBlock(exec && !takeIf);
      }
      return;
    }

    if (cur().type == TT::IDENT && cur().text == "while") {
      pos++;
      expectPunct("(");
      int condStart = pos;
      if (!exec) {
        parseOr(false);
        expectPunct(")");
        execBlock(false);
        return;
      }
      int iters = 0;
      while (!error && !returning) {
        pos = condStart;
        Value cond = parseOr(true);
        expectPunct(")");
        if (!cond.asBool()) { execBlock(false); break; }
        if (++iters > SCRIPT_LOOP_MAX_ITER) {
          fail("while loop exceeded " + String(SCRIPT_LOOP_MAX_ITER) + " iterations");
          break;
        }
        execBlock(true);
      }
      return;
    }

    if (cur().type == TT::IDENT && cur().text == "for") {
      pos++;
      expectPunct("(");
      execStatement(exec);
      if (error) return;
      int condStart = pos;

      int semiPos = scanToSemicolon(condStart);
      int incrStart = semiPos + 1;
      int closePos = scanToCloseParen(incrStart);
      int bodyPos = closePos + 1;
      if (!exec) {
        pos = bodyPos;
        execBlock(false);
        return;
      }
      int iters = 0;
      while (!error && !returning) {
        pos = condStart;
        Value cond = parseOr(true);

        if (!cond.asBool()) {
          pos = bodyPos;
          execBlock(false);
          break;
        }
        if (++iters > SCRIPT_LOOP_MAX_ITER) {
          fail("for loop exceeded " + String(SCRIPT_LOOP_MAX_ITER) + " iterations");
          break;
        }

        pos = bodyPos;
        execBlock(true);
        if (error || returning) break;

        pos = incrStart;
        execStatement(true);
      }
      return;
    }

    if (cur().type == TT::IDENT && cur().text == "return") {
      pos++;

      Value rv;
      if (!isEnd() &&
          !(cur().type == TT::PUNCT && (cur().text == ";" || cur().text == "}"))) {
        rv = parseOr(exec);
      }
      matchPunct(";");
      if (exec) { returning = true; returnValue = rv; }
      return;
    }

    if (cur().type == TT::IDENT && cur().text == "repeat") {
      pos++;
      expectPunct("(");
      Value countVal = parseOr(exec);
      expectPunct(")");
      int savedPos = pos;
      int n = exec ? (int)countVal.asNum() : 0;
      if (n < 0) n = 0;
      if (n > SCRIPT_LOOP_MAX_ITER) n = SCRIPT_LOOP_MAX_ITER;
      if (!exec || n == 0) { execBlock(false); return; }
      for (int i = 0; i < n && !error && !returning; i++) {
        pos = savedPos;
        execBlock(true);
      }
      return;
    }

    if (cur().type == TT::IDENT && cur().text == "function") {
      pos++;
      if (isEnd() || cur().type != TT::IDENT) { fail("expected function name after 'function'"); return; }
      String fname = cur().text; pos++;
      expectPunct("(");
      FuncDef fd;
      fd.used = true;
      fd.name = fname;
      fd.paramCount = 0;
      while (!error && !isEnd() && !(cur().type == TT::PUNCT && cur().text == ")")) {
        if (cur().type != TT::IDENT) { fail("expected parameter name in function declaration"); return; }
        if (fd.paramCount >= SCRIPT_FUNC_PARAMS_MAX) { fail("too many parameters (max " + String(SCRIPT_FUNC_PARAMS_MAX) + ")"); return; }
        fd.params[fd.paramCount++] = cur().text; pos++;
        matchPunct(",");
      }
      expectPunct(")");
      if (error) return;
      fd.bodyPos = pos;

      int fi = findFunc(fname);
      if (fi >= 0) {
        funcs[fi] = fd;
      } else if (funcCount < SCRIPT_FUNCS_MAX) {
        funcs[funcCount++] = fd;
      } else {
        fail("too many functions (max " + String(SCRIPT_FUNCS_MAX) + ")");
        return;
      }

      execBlock(false);
      return;
    }

    if (cur().type == TT::IDENT) {
      int savedPos = pos;
      String name = cur().text; pos++;

      if (!isEnd() && cur().type == TT::PUNCT && cur().text == "[") {
        int closeBracket = pos + 1;
        int depth = 1;
        while (closeBracket < count && depth > 0) {
          if (toks[closeBracket].type == TT::PUNCT) {
            if (toks[closeBracket].text == "[") depth++;
            else if (toks[closeBracket].text == "]") depth--;
          }
          if (depth > 0) closeBracket++;
        }
        bool isAssign = (closeBracket + 1 < count &&
                         toks[closeBracket + 1].type == TT::PUNCT &&
                         toks[closeBracket + 1].text == "=");
        if (isAssign) {
          pos++;
          Value idx = parseOr(exec);
          expectPunct("]");
          expectPunct("=");
          Value rhs = parseOr(exec);
          matchPunct(";");
          if (exec) {
            Value base = getVar(name);
            if (base.type == VT::ARRAY) {
              if (!arraySet(base.ref, (int)idx.asNum(), rhs))
                fail("array index out of bounds");
            } else if (base.type == VT::OBJECT) {
              objectSet(base.ref, idx.asStr(), rhs);
            }
          }
          return;
        }
        pos = savedPos;
      }

      else if (!isEnd() && cur().type == TT::PUNCT && cur().text == ".") {
        if (pos + 1 < count && toks[pos + 1].type == TT::IDENT &&
            pos + 2 < count && toks[pos + 2].type == TT::PUNCT &&
            toks[pos + 2].text == "=") {
          pos++;
          String field = cur().text; pos++;
          pos++;
          Value rhs = parseOr(exec);
          matchPunct(";");
          if (exec) {
            Value obj = getVar(name);
            if (obj.type == VT::OBJECT) objectSet(obj.ref, field, rhs);
          }
          return;
        }
        pos = savedPos;
      }

      else if (!isEnd() && cur().type == TT::PUNCT && cur().text == "=") {
        pos++;
        Value v = parseOr(exec);
        matchPunct(";");
        if (exec) setVar(name, v);
        return;
      } else {
        pos = savedPos;
      }
    }

    parseOr(exec);
    matchPunct(";");
  }

  void run() {
    pos = 0;
    stepBudget = 200000;
    while (!error && !isEnd()) {
      execStatement(true);
    }
  }
};

Token g_tokens[SCRIPT_TOKENS_MAX];

bool sanitizeName(const String& raw, String* out) {
  if (raw.length() == 0 || raw.length() > SCRIPT_NAME_MAX) return false;
  String s;
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-') s += c;
  }
  if (s.length() == 0) return false;
  *out = "/" + s + ".js";
  return true;
}

void cmdList() {
  File root = LittleFS.open("/");
  if (!root) { Serial.println("ERROR"); return; }
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (name.endsWith(".js")) {
      Serial.print("SCRIPT:");
      Serial.print(name.startsWith("/") ? name.substring(1) : name);
      Serial.print(" bytes:");
      Serial.println(f.size());
    }
    f = root.openNextFile();
  }
  Serial.println("SCRIPTLISTDONE");
}

void cmdSave(const String& rest) {
  int sep = rest.indexOf(':');
  if (sep < 0) { Serial.println("ERROR"); return; }
  String name, path;
  if (!sanitizeName(rest.substring(0, sep), &path)) { Serial.println("ERROR:BADNAME"); return; }
  String source = rest.substring(sep + 1);
  if ((int)source.length() > SCRIPT_SOURCE_MAX) { Serial.println("ERROR:TOOLONG"); return; }

  File f = LittleFS.open(path, "w");
  if (!f) { Serial.println("ERROR"); return; }
  f.print(source);
  f.close();
  Serial.println("OK");
}

void cmdShow(const String& rawName) {
  String path;
  if (!sanitizeName(rawName, &path)) { Serial.println("ERROR:BADNAME"); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.println("ERROR:NOTFOUND"); return; }
  Serial.println(f.readString());
  Serial.println("SCRIPTSHOWDONE");
  f.close();
}

void cmdDel(const String& rawName) {
  String path;
  if (!sanitizeName(rawName, &path)) { Serial.println("ERROR:BADNAME"); return; }
  Serial.println(LittleFS.remove(path) ? "OK" : "ERROR:NOTFOUND");
}

void cmdRun(const String& rawName) {
  String path;
  if (!sanitizeName(rawName, &path)) { Serial.println("ERROR:BADNAME"); return; }
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.println("ERROR:NOTFOUND"); return; }
  String source = f.readString();
  f.close();

  int tokenCount = tokenize(source, g_tokens, SCRIPT_TOKENS_MAX);
  if (tokenCount < 0) { Serial.println("ERROR:SCRIPTTOOLONG"); return; }

  Interp interp;
  interp.toks = g_tokens;
  interp.count = tokenCount;
  interp.run();

  if (interp.error) {
    Serial.print("ERROR:");
    Serial.println(interp.errMsg);
  }
  Serial.println("SCRIPTDONE");
}
}

namespace FoxScript {
void begin() {
  LittleFS.begin(true);
}

bool handleCommand(const String& line) {
  if (line == "SCRIPTLIST") { cmdList(); return true; }
  if (line.startsWith("SCRIPTSAVE:")) { cmdSave(line.substring(11)); return true; }
  if (line.startsWith("SCRIPTSHOW:")) { cmdShow(line.substring(11)); return true; }
  if (line.startsWith("SCRIPTDEL:")) { cmdDel(line.substring(10)); return true; }
  if (line.startsWith("SCRIPTRUN:")) { cmdRun(line.substring(10)); return true; }
  return false;
}
}
