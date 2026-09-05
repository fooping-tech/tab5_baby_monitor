/*
 * SPDX-License-Identifier: MIT
 *
 * Bounded RTSP/TCP input framing.  RTSP control messages and interleaved
 * RTCP share one byte stream, so the transport parser must not use C-string
 * searches across binary payloads.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tab5::rtsp {

constexpr size_t kMaxRtspRequestBytes = 2048;
constexpr size_t kMaxInterleavedPayloadBytes = 2048;

enum class InputFrameType : uint8_t {
    NeedMore,
    Request,
    Interleaved,
    Error,
};

struct InputFrame {
    InputFrameType type = InputFrameType::NeedMore;
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t wire_size = 0;
    uint8_t channel = 0;
};

class InputFramer {
public:
    // The buffer holds one bounded RTSP request or one bounded interleaved
    // frame.  The caller consumes a frame before appending more input.
    static constexpr size_t kBufferBytes = kMaxRtspRequestBytes + 4;

    size_t writable_capacity() const { return kBufferBytes - size_; }
    bool append(const uint8_t *data, size_t size);
    InputFrame next() const;
    void consume(size_t wire_size);
    bool has_error() const { return error_; }

private:
    uint8_t buffer_[kBufferBytes] = {};
    size_t size_ = 0;
    bool error_ = false;
};

// These helpers parse only the transport contract. They do not allocate and
// accept a length so they remain safe when called on a copied RTSP request.
bool request_has_tcp_transport(const uint8_t *request, size_t size);
bool parse_interleaved_channels(const uint8_t *request, size_t size,
                                uint8_t *rtp_channel, uint8_t *rtcp_channel);

} // namespace tab5::rtsp
