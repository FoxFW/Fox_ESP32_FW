#pragma once

#include <Arduino.h>

#include "fox_reply.h"

namespace FoxBle {
// NOT SINK-REDIRECTED (partially): BLEINIT/BLESTATUS/BLEDISC/BLESVC/
// BLECHAR/BLEWRITE/BLESCAN's own status lines are redirected to `out` like
// every other subsystem here, but the devices a BLESCAN finds (printed by
// ScanCallback::onResult, in ble_bridge.cpp) and any BLE notification
// payloads (notifyCallback, registered by BLECHAR: and fired later,
// asynchronously, whenever the peripheral sends one) still go to the
// physical Serial only - see the comments above those two in
// ble_bridge.cpp for why.
bool handleCommand(const String& line, Print& out = Serial);

bool writeHex(const String& hex);

bool isConnected();

void scriptScan();

bool ensureInitialized();
}
