#pragma once

#include <stdint.h>

#include "LineReader.h"
#include "Protocol.h"

namespace wire {

/// What the app implements to receive decoded frames.
class FrameHandler {
public:
    virtual ~FrameHandler() = default;
    virtual void onHello(const proto::InboundFrame& frame) = 0;
    virtual void onState(const proto::InboundFrame& frame) = 0;
    virtual void onMessage(const proto::InboundFrame& frame) = 0;
    virtual void onSessions(const proto::InboundFrame& frame) = 0;
    virtual void onPermission(const proto::InboundFrame& frame) = 0;
    virtual void onPermissionCleared(const proto::InboundFrame& frame) = 0;
    virtual void onError(const proto::InboundFrame& frame) = 0;
    virtual void onPing(int seq) = 0;
    virtual void onPong(int seq) = 0;
    virtual void onTime(const proto::InboundFrame& frame) = 0;
};

/// NDJSON over the USB-UART bridge.
///
/// Reads bytes into `LineReader`, decodes strictly, and dispatches. A line that
/// fails validation is logged and dropped: serial noise never becomes an
/// action.
class SerialProtocol {
public:
    void begin(FrameHandler& handler);

    /// Drains whatever bytes have arrived. Non-blocking.
    void poll();

    bool sendAction(proto::Action action, const char* requestId,
                    const char* sessionId = nullptr);
    bool sendNavigation(proto::Navigation navigation);
    /// Asks the Mac to report on `sessionId` instead of whichever session it
    /// was following.
    bool sendFocus(const char* sessionId);
    bool sendHello(const char* app, const char* version);
    bool sendPing(int seq);
    bool sendPong(int seq);
    /// Hands a one-time code to the Mac, which puts it on the clipboard.
    bool sendOtp(const char* label, const char* code, uint32_t expiresInMs);

    /// Frames accepted and rejected since boot, for diagnostics.
    uint32_t acceptedFrames() const { return accepted_; }
    uint32_t rejectedFrames() const { return rejected_; }

private:
    void dispatch(const char* line, size_t length);
    bool write(const char* frame, size_t length);

    FrameHandler* handler_ = nullptr;
    proto::LineReader reader_;
    proto::InboundFrame frame_;
    uint32_t accepted_ = 0;
    uint32_t rejected_ = 0;
};

}  // namespace wire
