#include "config.h"
#include "settings.h"
#include "ble_bridge.h"
#include "ble_attack.h"
#include "ble_tags.h"
#include "wifi_recon.h"
#include "wifi_attack.h"
#include "http_bridge.h"
#include "script_engine.h"
#include "rfid.h"
#include "subghz.h"
#include "ir.h"
#include "gps.h"
#include "fox_portal.h"
#include "discord.h"

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  Serial.println("Fox ESP32 Firmware v" FOX_FIRMWARE_VERSION " booted on UART0 (GPIO1/GPIO3)");

  FoxSettings::begin();
  FoxWifiRecon::begin();
  FoxHttp::begin();
  FoxScript::begin();
}

void handleCommand(const String& line) {
  if (line == "AT") {
    Serial.println("OK");
    return;
  }

  if (line == "info") {
    Serial.println("Fox ESP32 Firmware");
    return;
  }

  if (line == "CAPS") {
    Serial.print("HASBLE:");
    Serial.println(FOX_HAS_BLE ? "1" : "0");
    return;
  }

  if (FoxSettings::handleSettingsCommand(line)) return;
  if (FoxBle::handleCommand(line)) return;
  if (FoxBleAttack::handleCommand(line)) return;
  if (FoxBleTags::handleCommand(line)) return;
  if (FoxWifiRecon::handleCommand(line)) return;
  if (FoxWifiAttack::handleCommand(line)) return;
  if (FoxHttp::handleCommand(line)) return;
  if (FoxScript::handleCommand(line)) return;
  if (FoxRfid::handleCommand(line)) return;
  if (FoxSubGhz::handleCommand(line)) return;
  if (FoxIr::handleCommand(line)) return;
  if (FoxGps::handleCommand(line)) return;
  if (FoxPortal::handleCommand(line)) return;
  if (FoxDiscord::handleCommand(line)) return;

  if (line.length() > 0) {
    Serial.print("ECHO:");
    Serial.println(line);
  }
}

void loop() {
  static String line;

  FoxHttp::loop();
  FoxPortal::loop();
  FoxGps::loop();

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      line.trim();
      handleCommand(line);
      line = "";
    } else if (c != '\r') {
      if ((int)line.length() < LINE_BUFFER_MAX) line += c;
    }
  }
}
