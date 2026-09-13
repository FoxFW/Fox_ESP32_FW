#pragma once

// Companion (module) side of a real Flipper RPC session, run over the
// physical UART Fox_ESP32_FW already shares with every Fox app's plain
// AT-command traffic. Implements the same handshake/keep-alive/flow
// control a real Flipper WiFi Devboard does against the Flipper's stock
// applications/services/expansion/expansion_worker.c state machine, built
// on fox_rpc_frame.h's non-blocking frame reader/writer.
//
// Hard precondition (confirmed by reading FoxFW2.0's expansion.c/esp_at.c):
// the Flipper's stock Expansion service only listens on this UART while no
// Fox app has claimed it via esp_at_alloc()/expansion_disable() - i.e. the
// Flipper has to be sitting outside any Fox app (home screen or otherwise)
// for start() to have any chance of connecting. FoxLAB's [LAB/RPC/START]
// command (fox_lab.cpp) is what actually orchestrates that on the ESP32
// side; this module just speaks the wire protocol once asked to.
//
// Confirmed by reading targets/f7/furi_hal/furi_hal_serial_control.c
// (FoxFW2.0): module detection arms the Flipper's UART RX pin as a plain
// falling-edge GPIO interrupt while idle - there's no separate
// presence-detect pin, so simply transmitting the first bit of anything on
// the shared UART (the same 2-wire connection Fox's boards already use for
// AT commands) is the detection signal. start() sends exactly one trigger
// Heartbeat for this and does not resend it - see the file-header note on
// the Connecting state below for why a resend is actively dangerous here,
// not just redundant. What's still unverified without real hardware: the
// microsecond-scale timing margin between "Flipper sees the falling edge"
// and "Flipper's worker has finished re-arming its UART and is ready to
// receive the rest of our trigger frame" - if that ever proves too tight
// in practice, the fix is a deliberately tiny first transmission (even a
// single break/idle byte before the real Heartbeat) rather than anything
// structural here.
//
// Every public function here (other than pump()) is meant to be called
// from FoxLab's own logic, not from an ISR - there's no locking.

#include <Arduino.h>
#include "fox_rpc_frame.h"

namespace FoxRpcBridge {

enum class State {
  Idle,          // no session attempt in progress
  Connecting,    // retrying trigger heartbeats, waiting for the Flipper's own heartbeat
  Handshaking,   // sent our BaudRate proposal, waiting for a Status ack (still at 9600)
  Connected,     // baud switched, link established, no RPC session yet
  StartingRpc,   // sent Control(StartRpc), waiting for a Status ack
  RpcActive,     // RPC session open - relaying Data frame payloads
  Failed,        // gave up (timeout, rejected baud rate, or a protocol error) - call stop()
};

// Bytes the Flipper sent us via a Data frame while RpcActive, delivered as
// soon as each frame is decoded and acked. May be called with len==0 for
// an intentionally-empty Data frame - callers should just ignore that.
typedef void (*RpcRxCallback)(const uint8_t* data, size_t len);

// Called whenever state() changes, mainly so the [LAB/RPC/...] command
// layer and the WebSocket relay can react (report status, drop the socket
// on Failed, etc.) without polling state() every loop() tick themselves.
typedef void (*StateChangeCallback)(State newState);

// Starts (or restarts) a connection attempt: switches `uart` to
// EXPANSION_PROTOCOL_DEFAULT_BAUD_RATE immediately and begins the
// Connecting state. `targetBaud` is what we'll propose once the Flipper's
// heartbeat arrives - pass Fox_ESP32_FW's normal SERIAL_BAUD so the link
// ends up running at the same speed AT-mode already uses.
void start(HardwareSerial& uart, uint32_t targetBaud);

// Tears the session down: sends Control(StopRpc) first if RpcActive, then
// goes to Idle. Does not touch the UART's baud rate - the caller (fox_lab.cpp)
// owns restoring AT-mode framing/baud afterward.
void stop();

State state();

void setRpcRxCallback(RpcRxCallback cb);
void setStateChangeCallback(StateChangeCallback cb);

// Requests the actual RPC session once Connected. No-op (returns false) from
// any other state.
bool startRpc();

// Queues bytes to relay to the Flipper as Data frames (chunked into
// <=64-byte frames, one outstanding at a time per protocol, same as the
// Flipper's own outbound flow control). Returns false if not RpcActive, or
// if the outbound buffer is full - caller should back off and retry once
// pump() has drained more of it.
bool sendRpcBytes(const uint8_t* data, size_t len);

// How much room is left in the outbound buffer, for callers deciding
// whether to bother calling sendRpcBytes() yet.
size_t sendRpcBytesFree();

// Must be called every loop() tick while state() != Idle. Drains available
// UART bytes, drives the handshake/keep-alive/flow-control timers, and
// delivers inbound Data frame payloads via the RpcRxCallback.
void pump();

}  // namespace FoxRpcBridge
