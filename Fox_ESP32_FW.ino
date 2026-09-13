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
#include "fox_csi.h"
#include "gemini.h"
#include "fox_lab.h"
#include "fox_remote.h"
#include "fox_dispatch.h"

SET_LOOP_TASK_STACK_SIZE(32 * 1024);

void setup() {
  Serial.setRxBufferSize(LINE_BUFFER_MAX + 256);
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  Serial.println("Fox ESP32 Firmware v" FOX_FIRMWARE_VERSION " booted on UART0 (GPIO1/GPIO3)");

  FoxSettings::begin();
  FoxWifiRecon::begin();
  FoxHttp::begin();
  FoxScript::begin();
  FoxCsi::begin();
  FoxLab::begin();
}

void loop() {
  static String line;

  FoxHttp::loop();
  FoxPortal::loop();
  FoxGps::loop();
  FoxCsi::loop();
  FoxLab::loop();

  // A line starting with "[FLPR/" is a Fox Remote companion command reply
  // or push (device/power/storage info, file browsing, screen frames,
  // etc.) meant for every browser client currently attached to FoxLAB's
  // relay WebSocket (fox_lab.cpp/fox_remote.h) - relay it there instead of
  // through the normal AT-command dispatch chain. Everything else on this
  // shared UART goes through FoxDispatch::handleCommand() exactly as
  // before - unlike the old Expansion-Protocol-based RPC bridge this
  // replaced, the Fox Remote relay never touches the UART's baud rate or
  // framing, so this reader never has to step aside for it.
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      line.trim();
      if (line.startsWith("[FLPR/")) {
        FoxRemote::forwardToClient(line);
      } else {
        FoxDispatch::handleCommand(line);
      }
      line = "";
    } else if (c != '\r') {
      if ((int)line.length() < LINE_BUFFER_MAX) line += c;
    }
  }
}
