#include "ble_bridge.h"
#include "config.h"

#if FOX_HAS_BLE

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_bt.h>

#if !FOX_BLE_NIMBLE
#include <esp_gap_ble_api.h>
#endif

namespace {
BLEClient* bleClient = nullptr;
bool bleInitialized = false;
BLERemoteService* remoteService = nullptr;
BLERemoteCharacteristic* writeChar = nullptr;
BLERemoteCharacteristic* notifyChar = nullptr;

const char* addressTypeLabel(uint8_t type) {
  switch (type) {
    case 0: return "PUBLIC";
    case 1: return "RANDOM";
    case 2: return "RPA_PUBLIC";
    case 3: return "RPA_RANDOM";
    default: return "UNKNOWN";
  }
}

class ScanCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) {
    Serial.print("FOUND:");
    Serial.print(device.getAddress().toString().c_str());
    Serial.print(" type:");
    Serial.print(addressTypeLabel(device.getAddressType()));
    Serial.print("(");
    Serial.print(device.getAddressType());
    Serial.print(") rssi:");
    Serial.print(device.getRSSI());
    Serial.print(" name:");
    Serial.println(device.haveName() ? device.getName().c_str() : "");
  }
};

void printHex(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
}

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

void notifyCallback(BLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  Serial.print("NOTIFY:");
  printHex(data, length);
  Serial.println();
}
}

namespace FoxBle {
bool isConnected() {
  return bleClient != nullptr && bleClient->isConnected();
}

bool ensureInitialized() {
  if (!bleInitialized) {
    bleInitialized = BLEDevice::init("FoxESP32");
    if (bleInitialized) {
#if FOX_BLE_NIMBLE
      BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
      BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
      BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_SCAN);
#else
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P9);
#endif
    }
  }
  return bleInitialized;
}

bool writeHex(const String& hex) {
  if (writeChar == nullptr) return false;

  uint8_t buffer[HEX_BUFFER_MAX];
  size_t length = 0;
  if (!hexToBytes(hex, buffer, sizeof(buffer), &length)) return false;

  writeChar->writeValue(buffer, length, false);
  return true;
}

void scriptScan() {
  if (!bleInitialized) return;
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallback());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(BLE_SCAN_SECONDS, false);
  scan->clearResults();
}

bool handleCommand(const String& line) {
  if (line == "BLEINIT") {
    ensureInitialized();
    Serial.println(bleInitialized ? "OK" : "ERROR");
    return true;
  }

  if (line == "BLESCAN") {
    if (!bleInitialized) {
      Serial.println("ERROR");
      return true;
    }
    BLEScan* scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new ScanCallback());
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(BLE_SCAN_SECONDS, false);
    scan->clearResults();
    Serial.println("SCANDONE");
    return true;
  }

  if (line.startsWith("BLECONN:")) {
    if (!bleInitialized) {
      Serial.println("ERROR");
      return true;
    }

    String mac = line.substring(8);
    mac.trim();

    if (bleClient == nullptr) {
      bleClient = BLEDevice::createClient();
    }
    if (bleClient->isConnected()) {
      bleClient->disconnect();
    }
    remoteService = nullptr;
    writeChar = nullptr;
    notifyChar = nullptr;

#if FOX_BLE_NIMBLE
    BLEAddress addressPublic(mac.c_str(), BLE_ADDR_PUBLIC);
    bleClient->connect(addressPublic);
    if (!bleClient->isConnected()) {
      BLEAddress addressRandom(mac.c_str(), BLE_ADDR_RANDOM);
      bleClient->connect(addressRandom);
    }
#else
    BLEAddress address(mac.c_str());
    bleClient->connect(address, BLE_ADDR_TYPE_PUBLIC);
    if (!bleClient->isConnected()) {
      bleClient->connect(address, BLE_ADDR_TYPE_RANDOM);
    }
#endif

    Serial.println(bleClient->isConnected() ? "OK" : "ERROR");
    return true;
  }

  if (line == "BLESTATUS") {
    Serial.println(isConnected() ? "CONNECTED" : "DISCONNECTED");
    return true;
  }

  if (line == "BLEDISC") {
    if (bleClient != nullptr && bleClient->isConnected()) {
      bleClient->disconnect();
    }
    remoteService = nullptr;
    writeChar = nullptr;
    notifyChar = nullptr;
    Serial.println("OK");
    return true;
  }

  if (line.startsWith("BLESVC:")) {
    if (bleClient == nullptr || !bleClient->isConnected()) {
      Serial.println("ERROR");
      return true;
    }
    String uuid = line.substring(7);
    uuid.trim();
    remoteService = bleClient->getService(uuid.c_str());
    Serial.println(remoteService != nullptr ? "OK" : "ERROR");
    return true;
  }

  if (line.startsWith("BLECHAR:")) {
    if (remoteService == nullptr) {
      Serial.println("ERROR");
      return true;
    }
    String rest = line.substring(8);
    int comma = rest.indexOf(',');
    if (comma < 0) {
      Serial.println("ERROR");
      return true;
    }
    String writeUuid = rest.substring(0, comma);
    String notifyUuid = rest.substring(comma + 1);
    writeUuid.trim();
    notifyUuid.trim();

    writeChar = remoteService->getCharacteristic(writeUuid.c_str());
    notifyChar = remoteService->getCharacteristic(notifyUuid.c_str());

    if (writeChar == nullptr || notifyChar == nullptr) {
      Serial.println("ERROR");
      return true;
    }

    if (notifyChar->canNotify()) {
      notifyChar->registerForNotify(notifyCallback);
    }

    Serial.println("OK");
    return true;
  }

  if (line.startsWith("BLEWRITE:")) {
    if (writeChar == nullptr) {
      Serial.println("ERROR");
      return true;
    }
    String hex = line.substring(9);
    hex.trim();

    if (!writeHex(hex)) {
      Serial.println("ERROR");
      return true;
    }

    Serial.println("OK");
    return true;
  }

  return false;
}
}

#else

namespace FoxBle {
bool isConnected() { return false; }
bool ensureInitialized() { return false; }
bool writeHex(const String&) { return false; }
void scriptScan() {}

bool handleCommand(const String& line) {
  static const char* prefixes[] = {
    "BLEINIT", "BLESCAN", "BLECONN:", "BLESTATUS", "BLEDISC", "BLESVC:", "BLECHAR:", "BLEWRITE:"
  };
  for (auto p : prefixes) {
    if (line == p || line.startsWith(p)) {
      Serial.println("ERROR:Incompatible ESP32-S2 Module has no BLE");
      return true;
    }
  }
  return false;
}
}

#endif
