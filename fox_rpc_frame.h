#pragma once

// Non-blocking, incremental reader/writer for the Flipper Expansion
// Protocol (expansion_protocol.h, copied verbatim from FoxFW2.0's
// applications/services/expansion/ - Unlicense, meant to be dropped into
// any module firmware as-is). That header's struct layouts, size/checksum
// helpers are reused directly; only the blocking callback-loop functions
// (expansion_protocol_decode/encode) are NOT used, since they'd stall the
// whole Arduino loop() waiting on bytes. This file replaces those with a
// feed-one-byte-at-a-time reader plus a couple of small write helpers, so
// FoxLab::loop() can pump it alongside WiFi/WebServer/WebSocket work.
//
// Used by fox_rpc_bridge.cpp for the companion (module) side of a real
// Flipper RPC session, tunneled over the FoxHUB page's WebSocket instead
// of the physical UART a real WiFi Devboard would use for it.

#include <Arduino.h>
#include "expansion_protocol.h"

class ExpansionFrameReader {
 public:
  ExpansionFrameReader() { reset(); }

  void reset() {
    size_ = 0;
    haveChecksum_ = false;
  }

  // Feed one incoming byte. Returns true once a complete frame (payload +
  // checksum) has been accumulated and the checksum matches - the decoded
  // frame is available via frame() until the next feed() call after that
  // (call reset(), or just start feed()ing again - a fresh frame always
  // begins by calling reset() first). Returns false on a checksum mismatch
  // or a malformed/oversized frame; the caller should treat that as a
  // desync and reset() before continuing.
  //
  // On a checksum failure or malformed frame, out_error is set to true (if
  // non-null) - out_error is left untouched otherwise, since one feed()
  // call never touches it on a plain "still accumulating" outcome.
  bool feed(uint8_t byte, bool* out_error = nullptr) {
    if (!haveChecksum_) {
      size_t remaining = 0;
      if (size_ < sizeof(ExpansionFrame)) {
        reinterpret_cast<uint8_t*>(&frame_)[size_] = byte;
        size_++;
      } else {
        // Header/content claims more than a single frame can hold -
        // malformed. Never actually reachable given
        // EXPANSION_PROTOCOL_MAX_DATA_SIZE bounds checked below, but kept
        // as a hard backstop against a corrupt size byte.
        if (out_error) *out_error = true;
        return false;
      }

      if (!expansion_frame_get_remaining_size(&frame_, size_, &remaining)) {
        if (out_error) *out_error = true;
        return false;
      }
      if (remaining > 0) return false;

      // Frame content complete - next byte is the checksum.
      haveChecksum_ = true;
      return false;
    }

    // Checksum byte.
    uint8_t expected =
        expansion_protocol_get_checksum(reinterpret_cast<const uint8_t*>(&frame_), size_);
    haveChecksum_ = false;
    size_ = 0;
    if (byte != expected) {
      if (out_error) *out_error = true;
      return false;
    }
    return true;
  }

  const ExpansionFrame& frame() const { return frame_; }

 private:
  ExpansionFrame frame_;
  size_t size_ = 0;
  bool haveChecksum_ = false;
};

// Encodes `frame` and writes it (content bytes + trailing XOR checksum) to
// `out`. Returns false if the frame's header.type is invalid.
inline bool expansionFrameWrite(Print& out, const ExpansionFrame& frame) {
  size_t encodedSize = expansion_frame_get_encoded_size(&frame);
  if (encodedSize == 0) return false;

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&frame);
  out.write(bytes, encodedSize);
  out.write(expansion_protocol_get_checksum(bytes, encodedSize));
  return true;
}

inline ExpansionFrame expansionFrameHeartbeat() {
  ExpansionFrame frame;
  frame.header.type = ExpansionFrameTypeHeartbeat;
  return frame;
}

inline ExpansionFrame expansionFrameStatus(ExpansionFrameError error) {
  ExpansionFrame frame;
  frame.header.type = ExpansionFrameTypeStatus;
  frame.content.status.error = error;
  return frame;
}

inline ExpansionFrame expansionFrameBaudRate(uint32_t baud) {
  ExpansionFrame frame;
  frame.header.type = ExpansionFrameTypeBaudRate;
  frame.content.baud_rate.baud = baud;
  return frame;
}

inline ExpansionFrame expansionFrameControl(ExpansionFrameControlCommand command) {
  ExpansionFrame frame;
  frame.header.type = ExpansionFrameTypeControl;
  frame.content.control.command = command;
  return frame;
}

// data_size must be <= EXPANSION_PROTOCOL_MAX_DATA_SIZE (64); the caller is
// expected to chunk larger payloads across multiple frames.
inline ExpansionFrame expansionFrameData(const uint8_t* data, uint8_t data_size) {
  ExpansionFrame frame;
  frame.header.type = ExpansionFrameTypeData;
  frame.content.data.size = data_size;
  memcpy(frame.content.data.bytes, data, data_size);
  return frame;
}
