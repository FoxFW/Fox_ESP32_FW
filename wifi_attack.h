#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxWifiAttack {
bool handleCommand(const String& line, Print& out = Serial);

bool scriptDeauth();
bool scriptBeaconSpam(const String& ssid);

int scriptPortScan(const String& ip, int startPort, int endPort);
}
