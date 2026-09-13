#include "fox_rpc_bridge.h"

namespace FoxRpcBridge {

namespace {

// Send/receive timing. EXPANSION_PROTOCOL_TIMEOUT_MS (250, from
// expansion_protocol.h) is how long the FLIPPER's own worker waits before
// giving up - and this is stricter than it first looks: confirmed by
// reading FoxFW2.0's expansion_worker.c, the Flipper uses this exact
// timeout for EVERY byte it waits for, including the very first byte of a
// brand new frame, not just a continuation byte mid-frame. In other words
// the Flipper's worker unconditionally kills itself (exits its whole
// thread, requiring a fresh module-detect cycle to come back) if it goes
// 250ms without receiving ANYTHING from us at all, connected session or
// not - there is no separate "idle between frames is fine" allowance, and
// this side of the timing has no margin we can add, since it's fixed
// Flipper-firmware behavior we don't control from here.
// kAckTimeoutMs (below) is a different, separate wait - how long WE give
// the Flipper to transport-ack a frame we already finished sending
// (Handshaking's BaudRate ack, StartingRpc's Control(StartRpc) ack, and
// RpcActive's per-Data-frame ack) - and DOES have real margin to give,
// since the Flipper's own transport-level ack is sent immediately on
// receipt, before any slower RPC-session processing. It used to be set to
// EXPANSION_PROTOCOL_TIMEOUT_MS itself (250, zero margin) on the mistaken
// assumption it needed to race the Flipper's own per-byte timer; it
// doesn't, and having it be that tight was never actually the failure
// mode - see pump()'s RpcActive/awaitingDataAck_ handling for the real one
// this constant's old value was masking: while we wait out kAckTimeoutMs
// for an ack, we must still send the Flipper *something* at least every
// kHeartbeatIntervalMs, or its own 250ms rule above kills its worker out
// from under us regardless of how patient we're being on our end.
const uint32_t kHeartbeatIntervalMs = 100;
const uint32_t kAckTimeoutMs = 800;
const uint32_t kLinkWatchdogMs = kAckTimeoutMs * 3;
const uint32_t kConnectGiveUpMs = 5000;
const size_t kOutboundCapacity = 4096;

State state_ = State::Idle;
HardwareSerial* uart_ = nullptr;
uint32_t targetBaud_ = 0;

ExpansionFrameReader reader_;
RpcRxCallback rxCallback_ = nullptr;
StateChangeCallback stateChangeCallback_ = nullptr;

uint32_t lastRxMs_ = 0;
uint32_t lastTxMs_ = 0;
uint32_t connectStartMs_ = 0;
uint32_t handshakeSentMs_ = 0;
uint32_t startRpcSentMs_ = 0;

bool awaitingDataAck_ = false;
uint32_t dataAckSentMs_ = 0;

uint8_t outBuf_[kOutboundCapacity];
size_t outLen_ = 0;

void setState(State s) {
  state_ = s;
  if (stateChangeCallback_) stateChangeCallback_(s);
}

void setFailed() {
  setState(State::Failed);
}

void sendFrame(const ExpansionFrame& f) {
  expansionFrameWrite(*uart_, f);
  lastTxMs_ = millis();
}

size_t popOutbound(uint8_t* dst, size_t maxLen) {
  size_t n = outLen_ < maxLen ? outLen_ : maxLen;
  memcpy(dst, outBuf_, n);
  memmove(outBuf_, outBuf_ + n, outLen_ - n);
  outLen_ -= n;
  return n;
}

void handleFrame(const ExpansionFrame& f) {
  switch (state_) {
    case State::Connecting:
      // Anything arriving here means the Flipper's own worker just started
      // and is now blocking-waiting for our BaudRate proposal specifically
      // - any other reply at this point fails its handshake instantly, so
      // send exactly that and nothing else.
      sendFrame(expansionFrameBaudRate(targetBaud_));
      handshakeSentMs_ = millis();
      setState(State::Handshaking);
      break;

    case State::Handshaking:
      if (f.header.type == ExpansionFrameTypeStatus &&
          f.content.status.error == ExpansionFrameErrorNone) {
        uart_->flush();
        uart_->begin(targetBaud_);
        setState(State::Connected);
      } else {
        // Rejected (unsupported baud) or something unexpected - no
        // fallback baud strategy here; the caller can retry start() with a
        // different targetBaud if this turns out to matter on real
        // hardware.
        setFailed();
      }
      break;

    case State::Connected:
      if (f.header.type == ExpansionFrameTypeHeartbeat) {
        sendFrame(expansionFrameHeartbeat());
      } else {
        // Mirrors the Flipper's own connected-state handler: anything but
        // a Heartbeat here (we handle Control(StartRpc) via startRpc()
        // below, synchronously, not as a reply to an inbound frame) is a
        // protocol violation.
        setFailed();
      }
      break;

    case State::StartingRpc:
      if (f.header.type == ExpansionFrameTypeStatus &&
          f.content.status.error == ExpansionFrameErrorNone) {
        setState(State::RpcActive);
      } else if (f.header.type == ExpansionFrameTypeHeartbeat) {
        // A heartbeat the Flipper sent (or queued to send) before it had
        // processed our Control(StartRpc) can still be sitting in our
        // receive buffer when we get around to reading it - that's just
        // buffering delay on our end, not the Flipper doing anything
        // wrong, so answer it and keep waiting for the real ack instead
        // of failing the session over it.
        sendFrame(expansionFrameHeartbeat());
      } else {
        setFailed();
      }
      break;

    case State::RpcActive:
      if (f.header.type == ExpansionFrameTypeData) {
        sendFrame(expansionFrameStatus(ExpansionFrameErrorNone));
        if (rxCallback_) rxCallback_(f.content.data.bytes, f.content.data.size);
      } else if (f.header.type == ExpansionFrameTypeStatus) {
        if (f.content.status.error == ExpansionFrameErrorNone) {
          awaitingDataAck_ = false;
        } else {
          setFailed();
        }
      } else if (f.header.type == ExpansionFrameTypeHeartbeat) {
        sendFrame(expansionFrameHeartbeat());
      } else {
        setFailed();
      }
      break;

    default:
      break;
  }
}

}  // namespace

void start(HardwareSerial& uart, uint32_t targetBaud) {
  uart_ = &uart;
  targetBaud_ = targetBaud;
  reader_.reset();
  outLen_ = 0;
  awaitingDataAck_ = false;

  uart_->flush();
  uart_->begin(EXPANSION_PROTOCOL_DEFAULT_BAUD_RATE);

  uint32_t now = millis();
  connectStartMs_ = now;
  lastRxMs_ = now;

  setState(State::Connecting);

  // Send exactly one trigger byte - not a full frame. The Flipper's own
  // module-detection (targets/f7/furi_hal/furi_hal_serial_control.c) arms
  // its RX pin as a plain falling-edge GPIO interrupt while idle, with the
  // UART receiver itself disabled until the edge fires; only the first
  // bit's edge is guaranteed to land while it's actually watching for it,
  // not necessarily the rest of whatever byte we send it in. 0x00 can
  // never be mistaken for a real frame (valid ExpansionFrameType values
  // start at 1), so if it somehow does get decoded by an
  // already-listening Flipper it's harmless noise rather than a
  // misinterpreted frame. The real protocol conversation only starts once
  // the Flipper's own worker comes up and sends ITS heartbeat unprompted -
  // that's what Connecting's frame handling below is waiting for. Do NOT
  // resend this trigger periodically: by the time a retry would fire, the
  // Flipper's worker may already be up and blocking-waiting for our
  // BaudRate proposal specifically - anything else arriving in that window
  // (including a stray extra trigger of ours) fails its handshake
  // instantly.
  uart_->write((uint8_t)0x00);
  lastTxMs_ = now;
}

void stop() {
  if (state_ == State::RpcActive && uart_) {
    // Best-effort courtesy notice - if the link is already dead this just
    // goes nowhere, which is fine.
    sendFrame(expansionFrameControl(ExpansionFrameControlCommandStopRpc));
  }
  uart_ = nullptr;
  outLen_ = 0;
  awaitingDataAck_ = false;
  reader_.reset();
  setState(State::Idle);
}

State state() {
  return state_;
}

void setRpcRxCallback(RpcRxCallback cb) {
  rxCallback_ = cb;
}

void setStateChangeCallback(StateChangeCallback cb) {
  stateChangeCallback_ = cb;
}

bool startRpc() {
  if (state_ != State::Connected) return false;
  sendFrame(expansionFrameControl(ExpansionFrameControlCommandStartRpc));
  startRpcSentMs_ = millis();
  setState(State::StartingRpc);
  return true;
}

bool sendRpcBytes(const uint8_t* data, size_t len) {
  if (state_ != State::RpcActive) return false;
  if (len > kOutboundCapacity - outLen_) return false;
  memcpy(outBuf_ + outLen_, data, len);
  outLen_ += len;
  return true;
}

size_t sendRpcBytesFree() {
  return kOutboundCapacity - outLen_;
}

void pump() {
  if (state_ == State::Idle || state_ == State::Failed) return;
  if (!uart_) return;

  uint32_t now = millis();

  while (uart_->available() > 0) {
    uint8_t b = (uint8_t)uart_->read();
    bool err = false;
    bool complete = reader_.feed(b, &err);
    if (err) {
      reader_.reset();
      setFailed();
      return;
    }
    if (complete) {
      ExpansionFrame f = reader_.frame();
      reader_.reset();
      lastRxMs_ = now;
      handleFrame(f);
      if (state_ == State::Failed) return;
    }
  }

  switch (state_) {
    case State::Connecting:
      if (now - connectStartMs_ >= kConnectGiveUpMs) {
        setFailed();
        return;
      }
      break;

    case State::Handshaking:
      if (now - handshakeSentMs_ >= kAckTimeoutMs) {
        setFailed();
        return;
      }
      break;

    case State::Connected:
      if (now - lastTxMs_ >= kHeartbeatIntervalMs) {
        sendFrame(expansionFrameHeartbeat());
      }
      break;

    case State::StartingRpc:
      if (now - startRpcSentMs_ >= kAckTimeoutMs) {
        setFailed();
        return;
      }
      break;

    case State::RpcActive:
      if (awaitingDataAck_) {
        if (now - dataAckSentMs_ >= kAckTimeoutMs) {
          setFailed();
          return;
        }
        // Keep sending on our normal heartbeat cadence even while we wait
        // for the ack - see kHeartbeatIntervalMs's comment above. Skipping
        // this (as this code used to) risks the Flipper's own worker
        // hitting ITS 250ms silence limit and exiting on its own well
        // before our own, more generous kAckTimeoutMs would ever give up -
        // confirmed safe to interleave: RpcActive's Flipper-side handler
        // (expansion_worker_handle_state_rpc_active in expansion_worker.c)
        // accepts a Heartbeat frame at any point regardless of whatever
        // Data-frame ack it's also mid-sending us, and frames are decoded
        // strictly in the order their bytes arrive, so this can never
        // land ahead of - or otherwise disturb - the ack itself.
        if (now - lastTxMs_ >= kHeartbeatIntervalMs) {
          sendFrame(expansionFrameHeartbeat());
        }
      } else if (outLen_ > 0) {
        uint8_t chunk[EXPANSION_PROTOCOL_MAX_DATA_SIZE];
        size_t n = popOutbound(chunk, sizeof(chunk));
        sendFrame(expansionFrameData(chunk, (uint8_t)n));
        awaitingDataAck_ = true;
        dataAckSentMs_ = now;
      } else if (now - lastTxMs_ >= kHeartbeatIntervalMs) {
        sendFrame(expansionFrameHeartbeat());
      }
      break;

    default:
      break;
  }

  if ((state_ == State::Connected || state_ == State::RpcActive) &&
      (now - lastRxMs_ >= kLinkWatchdogMs)) {
    setFailed();
  }
}

}  // namespace FoxRpcBridge
