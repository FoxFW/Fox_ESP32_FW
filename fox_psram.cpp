#include "fox_psram.h"

#include "config.h"
#include <mbedtls/platform.h>
#include <string.h>

// IRAM0 FIX, ROUND 2 (2026-09-14, see claude/S2_RAM_OOM_ANALYSIS.md for the
// full diagnosis): round 1 swapped heap_caps_calloc()/heap_caps_free() for
// ps_malloc()/calloc()/free(), on the theory that heap_caps_* was pulling
// in IRAM-resident allocator internals. A real compile proved that theory
// wrong - the overflow was byte-for-byte identical either way. The actual
// linker map (the maintainer attached it) showed why: on classic
// specifically, NEITHER version of this file's code was the problem -
// classic's IRAM0 is already ~98% consumed by the full BTDM Bluetooth
// stack (libbtdm_app.a alone is ~33KB of the 128KB budget), and simply
// REFERENCING any PSRAM API anywhere in the firmware - psramFound(),
// ESP.getPsramSize(), ps_malloc() - pulls in ~3.7KB of libesp_psram.a's
// own IRAM-resident implementation code, which was never linked in before
// because nothing in Fox ever called those functions. That's what
// overflowed classic's IRAM0, regardless of which allocator backend this
// file used - see config.h's FOX_HAS_PSRAM_SUPPORT comment for the full
// board-by-board reasoning (this file makes ZERO PSRAM API calls on
// classic AND S3 now - classic for the IRAM reason above, S3 because its
// PSRAM comes in two incompatible bus widths that must match the
// physical module or risk a boot hang, and Marauder's own real-hardware
// builds ship every S3 target with PSRAM disabled). Only S2 and C5 keep
// the real fix - see config.h's per-board matrix for the full reasoning
// on every board, including why C3/C6 were never candidates at all (no
// PSRAM in silicon).
namespace {

bool psramReady = false;

#if FOX_HAS_PSRAM_SUPPORT
// mbedTLS's own memory hooks, overridden at RUNTIME via the documented
// mbedtls_platform_set_calloc_free() API (mbedtls/platform.h) - reachable
// because ESP-IDF's esp_config.h defines MBEDTLS_PLATFORM_MEMORY without
// the _MACRO variants that would otherwise hard-wire the allocator at
// compile time, for every value of the MBEDTLS_MEM_ALLOC_MODE Kconfig
// choice baked into the precompiled core (including the default
// MBEDTLS_INTERNAL_MEM_ALLOC every board ships with). See
// claude/S2_RAM_OOM_ANALYSIS.md for the full source/binary-level research
// trail confirming this symbol is present and reachable on the installed
// core. Falls back to internal RAM whenever PSRAM isn't ready or a PSRAM
// allocation fails for any reason (e.g. PSRAM fragmented/exhausted by
// something else) - never a hard dependency.
void* mbedtlsPsramCalloc(size_t n, size_t size) {
  size_t bytes = n * size;
  if (psramReady) {
    void* p = ps_malloc(bytes);
    if (p) {
      memset(p, 0, bytes);
      return p;
    }
  }
  return calloc(n, size);
}

void mbedtlsPsramFree(void* p) {
  free(p);
}
#endif  // FOX_HAS_PSRAM_SUPPORT

}  // namespace

namespace FoxPsram {

void begin() {
#if FOX_HAS_PSRAM_SUPPORT
  psramReady = psramFound() && (ESP.getPsramSize() > 0);

  Serial.print("PSRAM:");
  Serial.println(psramReady ? "FOUND" : "NONE");

  mbedtls_platform_set_calloc_free(mbedtlsPsramCalloc, mbedtlsPsramFree);
#else
  // Classic ESP32 only - see config.h's FOX_HAS_PSRAM_SUPPORT comment.
  // Deliberately calls nothing PSRAM-related at all: psramReady stays
  // false forever, mbedTLS keeps its default internal-RAM allocator
  // untouched, and every FoxPsram::alloc() call below falls straight
  // through to plain calloc() - byte-for-byte the same memory this
  // firmware already used on classic before this file existed.
  Serial.println("PSRAM:SKIPPED");
#endif
}

bool available() { return psramReady; }

void* alloc(size_t bytes) {
#if FOX_HAS_PSRAM_SUPPORT
  if (psramReady) {
    void* p = ps_malloc(bytes);
    if (p) {
      memset(p, 0, bytes);
      return p;
    }
  }
#endif
  return calloc(1, bytes);
}

}  // namespace FoxPsram
