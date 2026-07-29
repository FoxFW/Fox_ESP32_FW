#include "subghz.h"
#include "config.h"

#if FOX_HAS_SUBGHZ

#include <ELECHOUSE_CC1101_SRC_DRV.h>

namespace {
bool subghzInitialized = false;

bool hexCharToNibble(char c, uint8_t* out) {
  if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
  if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
  if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
  return false;
}

bool hexToBytes(const String& hex, uint8_t* out, size_t outCapacity, size_t* outLength) {
  size_t len = hex.length();
  if (len == 0 || len % 2 != 0) return false;
  size_t byteCount = len / 2;
  if (byteCount > outCapacity) return false;
  for (size_t i = 0; i < byteCount; i++) {
    uint8_t high, low;
    if (!hexCharToNibble(hex[i * 2], &high)) return false;
    if (!hexCharToNibble(hex[i * 2 + 1], &low)) return false;
    out[i] = (uint8_t)((high << 4) | low);
  }
  *outLength = byteCount;
  return true;
}

void printHex(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
}

void doInit() {
  ELECHOUSE_cc1101.setSpiPin(SUBGHZ_SCK_PIN, SUBGHZ_MISO_PIN, SUBGHZ_MOSI_PIN, SUBGHZ_CS_PIN);
  ELECHOUSE_cc1101.setGDO0(SUBGHZ_GDO0_PIN);
  ELECHOUSE_cc1101.Init();
  if (!ELECHOUSE_cc1101.getCC1101()) {
    Serial.println("ERROR:NOTFOUND");
    return;
  }
  ELECHOUSE_cc1101.setMHZ(SUBGHZ_DEFAULT_MHZ);
  subghzInitialized = true;
  Serial.println("OK");
}

void doFreq(float mhz) {
  if (mhz < 300.0f || mhz > 928.0f) {
    Serial.println("ERROR:BADFREQ");
    return;
  }
  ELECHOUSE_cc1101.setMHZ(mhz);
  Serial.println("OK");
}

void doRx() {
  ELECHOUSE_cc1101.SetRx();
  unsigned long start = millis();
  while (millis() - start < (unsigned long)SUBGHZ_RX_WAIT_SECONDS * 1000UL) {
    if (ELECHOUSE_cc1101.CheckRxFifo(100)) {
      uint8_t buf[SUBGHZ_RX_BUFFER_MAX];
      int len = ELECHOUSE_cc1101.ReceiveData(buf);
      if (len > 0) {
        Serial.print("SUBGHZRX:");
        printHex(buf, (size_t)min(len, (int)SUBGHZ_RX_BUFFER_MAX));
        Serial.println();
        return;
      }
    }
  }
  Serial.println("ERROR:NOSIGNAL");
}

void doTx(const String& hexStr) {
  uint8_t buf[SUBGHZ_RX_BUFFER_MAX];
  size_t length;
  if (!hexToBytes(hexStr, buf, sizeof(buf), &length)) {
    Serial.println("ERROR:BADHEX");
    return;
  }
  ELECHOUSE_cc1101.SetTx();
  ELECHOUSE_cc1101.SendData(buf, (int)length);
  ELECHOUSE_cc1101.SetRx();
  Serial.println("OK");
}
}

namespace FoxSubGhz {
bool handleCommand(const String& line) {
  if (line == "SUBGHZINIT") {
    doInit();
    return true;
  }

  if (line.startsWith("SUBGHZFREQ:") || line == "SUBGHZRX" || line.startsWith("SUBGHZTX:")) {
    if (!subghzInitialized) {
      Serial.println("ERROR:NOTINIT");
      return true;
    }
  }

  if (line.startsWith("SUBGHZFREQ:")) {
    float mhz = line.substring(strlen("SUBGHZFREQ:")).toFloat();
    doFreq(mhz);
    return true;
  }

  if (line == "SUBGHZRX") {
    doRx();
    return true;
  }

  if (line.startsWith("SUBGHZTX:")) {
    doTx(line.substring(strlen("SUBGHZTX:")));
    return true;
  }

  return false;
}
}

#else

namespace FoxSubGhz {
bool handleCommand(const String& line) {
  static const char* prefixes[] = {
    "SUBGHZINIT", "SUBGHZFREQ:", "SUBGHZRX", "SUBGHZTX:"
  };
  for (auto p : prefixes) {
    if (line == p || line.startsWith(p)) {
      Serial.println("ERROR:NOSUBGHZ");
      return true;
    }
  }
  return false;
}
}

#endif
