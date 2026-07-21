#include "rfid.h"
#include "config.h"

#if FOX_HAS_RFID

#include <Wire.h>
#include <Adafruit_PN532.h>

namespace {

Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RESET_PIN);
bool rfidInitialized = false;
const uint8_t MIFARE_DEFAULT_KEY[6] = RFID_MIFARE_DEFAULT_KEY;

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
  nfc.begin();
  uint32_t version = nfc.getFirmwareVersion();
  if (!version) {
    Serial.println("ERROR:NOTFOUND");
    return;
  }
  nfc.SAMConfig();
  rfidInitialized = true;
  Serial.println("OK");
}

bool scanOnce(uint8_t* uid, uint8_t* uidLength, uint16_t timeoutMs) {
  return nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLength, timeoutMs);
}

void doScan() {
  uint8_t uid[7];
  uint8_t uidLength;
  if (!scanOnce(uid, &uidLength, 2000)) {
    Serial.println("ERROR:NOTAG");
    return;
  }
  Serial.print("UID:");
  printHex(uid, uidLength);
  Serial.println();
}

void doRead(int block) {
  uint8_t uid[7];
  uint8_t uidLength;
  if (!scanOnce(uid, &uidLength, 2000)) {
    Serial.println("ERROR:NOTAG");
    return;
  }
  if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLength, (uint8_t)block, 0, (uint8_t*)MIFARE_DEFAULT_KEY)) {
    Serial.println("ERROR:AUTHFAILED");
    return;
  }
  uint8_t data[16];
  if (!nfc.mifareclassic_ReadDataBlock((uint8_t)block, data)) {
    Serial.println("ERROR:READFAILED");
    return;
  }
  Serial.print("BLOCK:");
  Serial.print(block);
  Serial.print(":");
  printHex(data, 16);
  Serial.println();
}

void doWrite(int block, const String& hexStr) {
  uint8_t data[16];
  size_t length;
  if (!hexToBytes(hexStr, data, sizeof(data), &length) || length != 16) {
    Serial.println("ERROR:BADHEX");
    return;
  }
  uint8_t uid[7];
  uint8_t uidLength;
  if (!scanOnce(uid, &uidLength, 2000)) {
    Serial.println("ERROR:NOTAG");
    return;
  }
  if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLength, (uint8_t)block, 0, (uint8_t*)MIFARE_DEFAULT_KEY)) {
    Serial.println("ERROR:AUTHFAILED");
    return;
  }
  if (!nfc.mifareclassic_WriteDataBlock((uint8_t)block, data)) {
    Serial.println("ERROR:WRITEFAILED");
    return;
  }
  Serial.println("OK");
}

}

namespace FoxRfid {

bool handleCommand(const String& line) {
  if (line == "RFIDINIT") {
    doInit();
    return true;
  }

  if (line == "RFIDSCAN" || line.startsWith("RFIDREAD:") || line.startsWith("RFIDWRITE:")) {
    if (!rfidInitialized) {
      Serial.println("ERROR:NOTINIT");
      return true;
    }
  }

  if (line == "RFIDSCAN") {
    doScan();
    return true;
  }

  if (line.startsWith("RFIDREAD:")) {
    int block = line.substring(strlen("RFIDREAD:")).toInt();
    doRead(block);
    return true;
  }

  if (line.startsWith("RFIDWRITE:")) {
    String rest = line.substring(strlen("RFIDWRITE:"));
    int c = rest.indexOf(':');
    if (c < 0) {
      Serial.println("ERROR:BADFORMAT");
      return true;
    }
    int block = rest.substring(0, c).toInt();
    String hexStr = rest.substring(c + 1);
    doWrite(block, hexStr);
    return true;
  }

  if (line.startsWith("RFIDEMULATE:")) {

    Serial.println("ERROR:NOTIMPLEMENTED");
    return true;
  }

  return false;
}

}

#else

namespace FoxRfid {

bool handleCommand(const String& line) {
  static const char* prefixes[] = {
    "RFIDINIT", "RFIDSCAN", "RFIDREAD:", "RFIDWRITE:", "RFIDEMULATE:"
  };
  for (auto p : prefixes) {
    if (line == p || line.startsWith(p)) {
      Serial.println("ERROR:NORFID");
      return true;
    }
  }
  return false;
}

}

#endif
