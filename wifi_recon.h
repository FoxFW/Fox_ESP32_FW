#pragma once

#include <Arduino.h>

namespace FoxWifiRecon {

void begin();

bool handleCommand(const String& line);

bool getSelectedAp(uint8_t bssidOut[6], uint8_t* channelOut, String* ssidOut);
bool getSelectedSta(uint8_t macOut[6]);

int scriptScanApCount();

}
