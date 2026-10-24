#include "SerialProtocol.h"

#include <Arduino.h>

#include "Config.h"
#include "Log.h"

namespace wire {
namespace {

/// Outbound frames are short; the buffer is sized for the longest one plus
/// escaping headroom.
constexpr size_t kOutboundCapacity = 256;

/// Bytes drained per `poll`, so a flood cannot starve the input and display
/// loops.
constexpr size_t kMaxBytesPerPoll = 512;

}  // namespace

void SerialProtocol::begin(FrameHandler& handler) {
    handler_ = &handler;
    reader_.reset();
}

void SerialProtocol::poll() {
    size_t budget = kMaxBytesPerPoll;
    while (Serial.available() > 0 && budget-- > 0) {
        const int value = Serial.read();
        if (value < 0) break;
        if (reader_.feed(static_cast<char>(value))) {
            dispatch(reader_.line(), reader_.length());
        }
    }

    static uint32_t reportedDrops = 0;
    if (reader_.droppedLines() != reportedDrops) {
        reportedDrops = reader_.droppedLines();
        logf(proto::LogLevel::Warn, "[serial] dropped %lu oversized line(s)",
             static_cast<unsigned long>(reportedDrops));
    }
}

void SerialProtocol::dispatch(const char* line, size_t length) {
    const proto::ParseResult result = proto::parseInbound(line, length, frame_);
    if (result != proto::ParseResult::Ok) {
        ++rejected_;
        // Logged, never silently ignored — and never acted on.
        logf(proto::LogLevel::Warn, "[serial] rejected frame: %s", proto::toString(result));
        return;
    }
    ++accepted_;
    if (handler_ == nullptr) return;

    switch (frame_.type) {
        case proto::InboundType::Hello: handler_->onHello(frame_); break;
        case proto::InboundType::State: handler_->onState(frame_); break;
        case proto::InboundType::Message: handler_->onMessage(frame_); break;
        case proto::InboundType::Sessions: handler_->onSessions(frame_); break;
        case proto::InboundType::Permission: handler_->onPermission(frame_); break;
        case proto::InboundType::PermissionCleared: handler_->onPermissionCleared(frame_); break;
        case proto::InboundType::Error: handler_->onError(frame_); break;
        case proto::InboundType::Ping: handler_->onPing(frame_.seq); break;
        case proto::InboundType::Pong: handler_->onPong(frame_.seq); break;
        case proto::InboundType::Time: handler_->onTime(frame_); break;
        case proto::InboundType::Unknown: break;
    }
}

bool SerialProtocol::write(const char* frame, size_t length) {
    if (length == 0) return false;
    Serial.write(reinterpret_cast<const uint8_t*>(frame), length);
    return true;
}

bool SerialProtocol::sendAction(
    proto::Action action, const char* requestId, const char* sessionId) {
    char frame[kOutboundCapacity];
    const size_t length = proto::encodeAction(frame, sizeof(frame), action, requestId, sessionId);
    if (length == 0) {
        logf(proto::LogLevel::Error, "[serial] refused to encode %s (bad or missing requestId)",
             proto::toString(action));
        return false;
    }
    return write(frame, length);
}

bool SerialProtocol::sendNavigation(proto::Navigation navigation) {
    char frame[kOutboundCapacity];
    return write(frame, proto::encodeNavigation(frame, sizeof(frame), navigation));
}

bool SerialProtocol::sendFocus(const char* sessionId) {
    char frame[kOutboundCapacity];
    const size_t length = proto::encodeFocus(frame, sizeof(frame), sessionId);
    if (length == 0) {
        logMessage(proto::LogLevel::Warn, "[serial] refused to encode focus (bad session id)");
        return false;
    }
    return write(frame, length);
}

bool SerialProtocol::sendHello(const char* app, const char* version) {
    char frame[kOutboundCapacity];
    return write(frame, proto::encodeHello(frame, sizeof(frame), app, version));
}

bool SerialProtocol::sendOtp(const char* label, const char* code, uint32_t expiresInMs) {
    char frame[kOutboundCapacity];
    return write(frame, proto::encodeOtp(frame, sizeof(frame), label, code, expiresInMs));
}

bool SerialProtocol::sendPing(int seq) {
    char frame[kOutboundCapacity];
    return write(frame, proto::encodePing(frame, sizeof(frame), seq));
}

bool SerialProtocol::sendPong(int seq) {
    char frame[kOutboundCapacity];
    return write(frame, proto::encodePong(frame, sizeof(frame), seq));
}

}  // namespace wire
