#pragma once

#include <Arduino.h>

// S2 RAM/OOM investigation (2026-09-14, see claude/S2_RAM_OOM_ANALYSIS.md
// project doc for the full research trail): a small, centralized helper for
// opportunistically moving fixed RAM costs off internal SRAM and onto
// external PSRAM, on any board that actually has it. This is NOT a new
// requirement - every allocator below falls back to the exact same internal
// RAM every board already used before this file existed, so a board with no
// PSRAM (or with the Tools -> PSRAM menu option left "Disabled") behaves
// identically to the pre-PSRAM firmware. Nothing here needs PlatformIO or a
// modified core - it's plain Arduino IDE + the mbedTLS platform.h API
// (mbedtls_platform_set_calloc_free) that ESP-IDF's own esp_config.h leaves
// deliberately reachable from application code.
//
// PSRAM silicon support, confirmed directly from arduino-esp32's own
// boards.txt (Tools -> PSRAM menu presence) and ESP-IDF's soc_caps.h
// (SOC_SPIRAM_SUPPORTED), per board Fox actually ships:
//   classic ESP32 - yes (hardware-dependent: WROVER modules have it,
//                   WROOM modules don't - the Tools menu option exists
//                   either way, this code only takes effect if real PSRAM
//                   is found at boot)
//   ESP32-S2       - yes (the WiFi Dev Board specifically has 2MB)
//   ESP32-S3       - yes (many dev modules ship with 8MB standard)
//   ESP32-C5       - yes
//   ESP32-C3       - no PSRAM support in silicon at all - this module is a
//                    guaranteed no-op there, always falling back to
//                    internal RAM
//   ESP32-C6       - no PSRAM support in silicon at all - same as C3
namespace FoxPsram {

// Call exactly once, as the very first thing in setup() - before
// FoxSettings/FoxWifiRecon/FoxHttp/FoxScript/FoxCsi/FoxLab::begin(), and
// critically before any WiFiClientSecure/HTTPClient TLS connection is ever
// opened (Discord/Gemini/GitHub check-in). Detects real PSRAM and, if
// found, redirects mbedTLS's own internal calloc/free (the ~32KB fixed
// RX/TX buffer cost paid by every TLS connection on every board) to PSRAM.
// This override is global - it affects every mbedTLS allocation in the
// firmware for the rest of runtime, which is intentional since nothing
// else in Fox uses mbedTLS directly besides the TLS clients this exists to
// help.
void begin();

// True once begin() has run and real PSRAM was found and initialized.
// Not required for correctness anywhere else in the firmware - every
// allocator here already has its own internal-RAM fallback - but useful
// for status/debug reporting (e.g. a future CAPS-style line).
bool available();

// General-purpose allocator for any other fixed buffer that's worth moving
// off internal RAM: tries PSRAM first (only when available() is true),
// falls back to normal internal-heap allocation otherwise, so this never
// returns null unless the firmware is completely out of memory either way
// - the same failure mode every static/malloc'd buffer already had before
// this file existed. Zero-initializes the result, like calloc(). For POD
// buffers only (uint8_t[]/char[] etc.) - anything containing a
// constructor-requiring type (Arduino String, etc.) needs placement-new
// after allocating, see script_engine.cpp's allocScriptTables() for that
// pattern.
void* alloc(size_t bytes);

}  // namespace FoxPsram
