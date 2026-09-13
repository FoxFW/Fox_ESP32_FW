#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxWifiRecon {
void begin();

// NOT SINK-REDIRECTED (partially): promiscuousCallback (wifi_recon.cpp) is
// the WiFi driver's own RX callback - live sniff/wardrive-adjacent output
// it prints directly (BEACON/DEAUTH/PROBEREQ/RAW/MULTISSID/PMKID/SAE/
// PCAPPKT+PCAPDATA lines) stays on the physical Serial only, whatever
// `out` a command was issued with. Everything handleCommand() prints
// itself - scan results, DONE/ERROR replies, WIFIPACKETCOUNT's COUNT:,
// WIFIMACTRACK's MACTRACK: lines (polled synchronously, not printed from
// the callback) - is redirected normally.
bool handleCommand(const String& line, Print& out = Serial);

bool getSelectedAp(uint8_t bssidOut[6], uint8_t* channelOut, String* ssidOut);
bool getSelectedSta(uint8_t macOut[6]);

int scriptScanApCount();
}
