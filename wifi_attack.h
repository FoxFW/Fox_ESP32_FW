#pragma once

#include <Arduino.h>

namespace FoxWifiAttack {
bool handleCommand(const String& line);

bool scriptDeauth();
bool scriptBeaconSpam(const String& ssid);

int scriptPortScan(const String& ip, int startPort, int endPort);
}
