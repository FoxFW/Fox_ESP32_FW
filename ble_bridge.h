#pragma once

#include <Arduino.h>

namespace FoxBle {

bool handleCommand(const String& line);

bool writeHex(const String& hex);

bool isConnected();

void scriptScan();

bool ensureInitialized();

}
