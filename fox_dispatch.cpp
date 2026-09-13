#include "fox_dispatch.h"
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

// Split out of Fox_ESP32_FW.ino (task #10) so a plain .cpp file - not the
// .ino itself - owns this definition. The Arduino IDE's automatic function-
// prototype insertion scans the .ino for top-level function signatures and
// has known trouble with functions defined inside a namespace block there;
// keeping this in an ordinary .cpp sidesteps that entirely, the same way
// every other subsystem's handleCommand() already does.

namespace FoxDispatch {

void handleCommand(const String& line, Print& out) {
#pragma push_macro("Serial")
#undef Serial
#define Serial out
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

  if (FoxSettings::handleSettingsCommand(line, out)) return;
  if (FoxBle::handleCommand(line, out)) return;
  if (FoxBleAttack::handleCommand(line, out)) return;
  if (FoxBleTags::handleCommand(line, out)) return;
  if (FoxWifiRecon::handleCommand(line, out)) return;
  if (FoxWifiAttack::handleCommand(line, out)) return;
  if (FoxHttp::handleCommand(line, out)) return;
  if (FoxScript::handleCommand(line, out)) return;
  if (FoxRfid::handleCommand(line, out)) return;
  if (FoxSubGhz::handleCommand(line, out)) return;
  if (FoxIr::handleCommand(line, out)) return;
  if (FoxGps::handleCommand(line, out)) return;
  if (FoxPortal::handleCommand(line, out)) return;
  if (FoxDiscord::handleCommand(line, out)) return;
  if (FoxCsi::handleCommand(line, out)) return;
  if (FoxGemini::handleCommand(line, out)) return;
  if (FoxLab::handleCommand(line, out)) return;

  if (line.length() > 0) {
    Serial.print("ECHO:");
    Serial.println(line);
  }
#undef Serial
#pragma pop_macro("Serial")
}

}  // namespace FoxDispatch
