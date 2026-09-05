/*
 * SPDX-License-Identifier: MIT
 */
#include "rtsp_framing.h"

#include <ctype.h>
#include <string.h>

namespace tab5::rtsp {
namespace {

static bool ascii_equal_insensitive(uint8_t actual, char expected)
{
    return static_cast<uint8_t>(tolower(static_cast<unsigned char>(actual))) ==
           static_cast<uint8_t>(tolower(static_cast<unsigned char>(expected)));
}

static bool matches_at(const uint8_t *data, size_t size, size_t offset, const char *needle)
{
    for (size_t i = 0; needle[i] != '\0'; ++i) {
        if (offset + i >= size || !ascii_equal_insensitive(data[offset + i], needle[i])) {
            return false;
        }
    }
    return true;
}

static size_t find_token(const uint8_t *data, size_t size, const char *needle)
{
    for (size_t offset = 0; offset < size; ++offset) {
        if (matches_at(data, size, offset, needle)) {
            return offset;
        }
    }
    return size;
}

static bool parse_u8(const uint8_t *data, size_t size, size_t *offset, uint8_t *value)
{
    if (offset == nullptr || value == nullptr) {
        return false;
    }
    size_t cursor = *offset;
    unsigned number = 0;
    bool found_digit = false;
    while (cursor < size && data[cursor] >= '0' && data[cursor] <= '9') {
        found_digit = true;
        const unsigned digit = static_cast<unsigned>(data[cursor] - '0');
        if (number > (255U - digit) / 10U) {
            return false;
        }
        number = number * 10U + digit;
        ++cursor;
    }
    if (!found_digit) {
        return false;
    }
    *offset = cursor;
    *value = static_cast<uint8_t>(number);
    return true;
}

} // namespace

bool InputFramer::append(const uint8_t *data, size_t size)
{
    if (error_ || (data == nullptr && size != 0) || size > writable_capacity()) {
        error_ = true;
        return false;
    }
    if (size > 0) {
        memcpy(buffer_ + size_, data, size);
        size_ += size;
    }
    return true;
}

InputFrame InputFramer::next() const
{
    InputFrame frame;
    if (error_ || size_ == 0) {
        frame.type = error_ ? InputFrameType::Error : InputFrameType::NeedMore;
        return frame;
    }

    if (buffer_[0] == '$') {
        if (size_ < 4) {
            return frame;
        }
        const size_t payload_size = (static_cast<size_t>(buffer_[2]) << 8) | buffer_[3];
        if (payload_size > kMaxInterleavedPayloadBytes) {
            frame.type = InputFrameType::Error;
            return frame;
        }
        if (size_ < payload_size + 4) {
            return frame;
        }
        frame.type = InputFrameType::Interleaved;
        frame.channel = buffer_[1];
        frame.data = buffer_ + 4;
        frame.size = payload_size;
        frame.wire_size = payload_size + 4;
        return frame;
    }

    for (size_t offset = 0; offset + 4 <= size_; ++offset) {
        if (buffer_[offset] != '\r' || buffer_[offset + 1] != '\n' ||
            buffer_[offset + 2] != '\r' || buffer_[offset + 3] != '\n') {
            continue;
        }
        const size_t request_size = offset + 4;
        if (request_size > kMaxRtspRequestBytes) {
            frame.type = InputFrameType::Error;
            return frame;
        }
        frame.type = InputFrameType::Request;
        frame.data = buffer_;
        frame.size = request_size;
        frame.wire_size = request_size;
        return frame;
    }
    if (size_ > kMaxRtspRequestBytes) {
        frame.type = InputFrameType::Error;
    }
    return frame;
}

void InputFramer::consume(size_t wire_size)
{
    if (wire_size == 0 || wire_size > size_) {
        error_ = true;
        return;
    }
    const size_t remaining = size_ - wire_size;
    memmove(buffer_, buffer_ + wire_size, remaining);
    size_ = remaining;
}

bool request_has_tcp_transport(const uint8_t *request, size_t size)
{
    return request != nullptr && find_token(request, size, "rtp/avp/tcp") < size;
}

bool parse_interleaved_channels(const uint8_t *request, size_t size,
                                uint8_t *rtp_channel, uint8_t *rtcp_channel)
{
    if (request == nullptr || rtp_channel == nullptr || rtcp_channel == nullptr) {
        return false;
    }
    uint8_t rtp = 0;
    uint8_t rtcp = 1;
    const size_t token = find_token(request, size, "interleaved");
    if (token < size) {
        size_t cursor = token + strlen("interleaved");
        while (cursor < size && (request[cursor] == ' ' || request[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= size || request[cursor] != '=') {
            return false;
        }
        ++cursor;
        while (cursor < size && (request[cursor] == ' ' || request[cursor] == '\t')) {
            ++cursor;
        }
        if (!parse_u8(request, size, &cursor, &rtp)) {
            return false;
        }
        while (cursor < size && (request[cursor] == ' ' || request[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= size || request[cursor] != '-') {
            return false;
        }
        ++cursor;
        while (cursor < size && (request[cursor] == ' ' || request[cursor] == '\t')) {
            ++cursor;
        }
        if (!parse_u8(request, size, &cursor, &rtcp)) {
            return false;
        }
    }
    if (rtp == rtcp) {
        return false;
    }
    *rtp_channel = rtp;
    *rtcp_channel = rtcp;
    return true;
}

} // namespace tab5::rtsp
