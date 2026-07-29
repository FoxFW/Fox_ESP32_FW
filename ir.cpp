#include "ir.h"
#include "config.h"

#if FOX_HAS_IR

#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

namespace {
IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN, IR_RECV_BUFFER_SIZE, IR_RECV_GAP_TIMEOUT_MS, true);
bool irInitialized = false;

struct TvBGoneCode {
  const char* label;
  decode_type_t protocol;
  uint64_t data;
  uint16_t bits;
};

const TvBGoneCode TVBGONE_CODES[] = {
  {"Samsung(NEC)", NEC,  0xE0E040BFULL, 32},
  {"LG(NEC)",      NEC,  0x20DF10EFULL, 32},
  {"Sony",         SONY, 0xA90ULL,      12},
};

void ensureInit() {
  if (irInitialized) return;
  irsend.begin();
  irrecv.enableIRIn();
  irInitialized = true;
}

void doSend(const String& protocolStr, const String& hexStr, int bits) {
  ensureInit();
  decode_type_t type = strToDecodeType(protocolStr.c_str());
  if (type == UNKNOWN) {
    Serial.println("ERROR:BADPROTOCOL");
    return;
  }
  if (hexStr.length() == 0) {
    Serial.println("ERROR:BADFORMAT");
    return;
  }
  uint64_t data = strtoull(hexStr.c_str(), nullptr, 16);
  if (bits <= 0) bits = 32;
  irsend.send(type, data, (uint16_t)bits);
  Serial.println("OK");
}

void doRecv() {
  ensureInit();
  decode_results results;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)IR_RECV_WAIT_SECONDS * 1000UL) {
    if (irrecv.decode(&results)) {
      char hexBuf[24];
      snprintf(hexBuf, sizeof(hexBuf), "%llX", (unsigned long long)results.value);
      Serial.print("IRRECV:");
      Serial.print(typeToString(results.decode_type));
      Serial.print(":0x");
      Serial.print(hexBuf);
      Serial.print(":");
      Serial.println(results.bits);
      irrecv.resume();
      return;
    }
    delay(10);
  }
  Serial.println("ERROR:NOSIGNAL");
}

void doTvBGone() {
  ensureInit();
  int count = (int)(sizeof(TVBGONE_CODES) / sizeof(TVBGONE_CODES[0]));
  for (int i = 0; i < count; i++) {
    Serial.print("TVBGONE:sending:");
    Serial.println(TVBGONE_CODES[i].label);
    irsend.send(TVBGONE_CODES[i].protocol, TVBGONE_CODES[i].data, TVBGONE_CODES[i].bits);
    delay(200);
  }
  Serial.println("TVBGONEDONE");
}
}

namespace FoxIr {
bool handleCommand(const String& line) {
  if (line.startsWith("IRSEND:")) {
    String rest = line.substring(strlen("IRSEND:"));
    int c1 = rest.indexOf(':');
    if (c1 < 0) {
      Serial.println("ERROR:BADFORMAT");
      return true;
    }
    String protocol = rest.substring(0, c1);
    String hexAndBits = rest.substring(c1 + 1);
    int c2 = hexAndBits.indexOf(':');
    String hexStr = (c2 < 0) ? hexAndBits : hexAndBits.substring(0, c2);
    int bits = (c2 < 0) ? 0 : hexAndBits.substring(c2 + 1).toInt();
    doSend(protocol, hexStr, bits);
    return true;
  }

  if (line == "IRRECV") {
    doRecv();
    return true;
  }

  if (line == "IRTVBGONE") {
    doTvBGone();
    return true;
  }

  return false;
}
}

#else

namespace FoxIr {
bool handleCommand(const String& line) {
  static const char* prefixes[] = {
    "IRSEND:", "IRRECV", "IRTVBGONE"
  };
  for (auto p : prefixes) {
    if (line == p || line.startsWith(p)) {
      Serial.println("ERROR:NOIR");
      return true;
    }
  }
  return false;
}
}

#endif
