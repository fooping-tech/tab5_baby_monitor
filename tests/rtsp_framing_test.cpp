/*
 * SPDX-License-Identifier: MIT
 */
#include "rtsp_framing.h"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void test_fragmented_request()
{
    tab5::rtsp::InputFramer framer;
    const uint8_t first[] = "DESCRIBE rtsp://tab5.local:8554/baby RTSP/1.0\r\nCSeq: 1\r\n";
    const uint8_t second[] = "\r\n";
    check(framer.append(first, sizeof(first) - 1), "append first request fragment");
    check(framer.next().type == tab5::rtsp::InputFrameType::NeedMore, "fragmented request waits");
    check(framer.append(second, sizeof(second) - 1), "append final request fragment");
    const auto frame = framer.next();
    check(frame.type == tab5::rtsp::InputFrameType::Request, "request is framed");
    check(frame.size == sizeof(first) + sizeof(second) - 2, "request size is exact");
    framer.consume(frame.wire_size);
    check(framer.next().type == tab5::rtsp::InputFrameType::NeedMore, "request is consumed");
}

void test_interleaved_binary_and_concatenation()
{
    tab5::rtsp::InputFramer framer;
    const uint8_t first[] = {'$', 7, 0, 3, 0, 0x80};
    const uint8_t second[] = {0xff, '$', 7, 0, 1, 0x81};
    check(framer.append(first, sizeof(first)), "append partial interleaved frame");
    auto frame = framer.next();
    check(frame.type == tab5::rtsp::InputFrameType::NeedMore, "partial binary frame waits");
    check(framer.append(second, sizeof(second)), "append concatenated binary frame");
    frame = framer.next();
    check(frame.type == tab5::rtsp::InputFrameType::Interleaved, "binary frame is framed");
    check(frame.channel == 7 && frame.size == 3 && frame.data[1] == 0x80,
          "binary channel and payload are preserved");
    framer.consume(frame.wire_size);
    frame = framer.next();
    check(frame.type == tab5::rtsp::InputFrameType::Interleaved && frame.channel == 7 && frame.size == 1,
          "second binary frame is not lost");
}

void test_binary_zero_and_text_boundary()
{
    tab5::rtsp::InputFramer framer;
    const uint8_t input[] = {'$', 2, 0, 2, 0, '\0', 'O', 'P', 'T', 'I', 'O', 'N', 'S', ' ', '*', ' ',
                             'R', 'T', 'S', 'P', '/', '1', '.', '0', '\r', '\n', 'C', 'S', 'e', 'q', ':', ' ',
                             '2', '\r', '\n', '\r', '\n'};
    check(framer.append(input, sizeof(input)), "append binary zero and text");
    auto frame = framer.next();
    check(frame.type == tab5::rtsp::InputFrameType::Interleaved && frame.size == 2,
          "binary payload with zero is not treated as text");
    framer.consume(frame.wire_size);
    frame = framer.next();
    check(frame.type == tab5::rtsp::InputFrameType::Request && frame.size > 0,
          "text after binary is framed");
}

void test_limits()
{
    tab5::rtsp::InputFramer framer;
    uint8_t oversized[tab5::rtsp::kMaxRtspRequestBytes + 1] = {};
    memset(oversized, 'X', sizeof(oversized));
    check(framer.append(oversized, sizeof(oversized)), "append request-sized input buffer");
    check(framer.next().type == tab5::rtsp::InputFrameType::Error, "unterminated oversized request rejected");

    tab5::rtsp::InputFramer interleaved;
    const uint8_t header[] = {'$', 1, 0x08, 0x01};
    check(interleaved.append(header, sizeof(header)), "append oversized binary header");
    check(interleaved.next().type == tab5::rtsp::InputFrameType::Error, "oversized binary frame rejected");
}

void test_transport_helpers()
{
    const char request[] = "SETUP rtsp://tab5.local/baby/trackID=0 RTSP/1.0\r\n"
                           "Transport: rtp/avp/tcp;unicast;interleaved=7-11\r\n\r\n";
    uint8_t rtp = 0;
    uint8_t rtcp = 0;
    check(tab5::rtsp::request_has_tcp_transport(reinterpret_cast<const uint8_t *>(request), strlen(request)),
          "TCP transport is case insensitive");
    check(tab5::rtsp::parse_interleaved_channels(reinterpret_cast<const uint8_t *>(request), strlen(request), &rtp,
                                                 &rtcp) && rtp == 7 && rtcp == 11,
          "interleaved channels are parsed");
    const char invalid[] = "Transport: RTP/AVP/TCP;interleaved=4-4\r\n\r\n";
    check(!tab5::rtsp::parse_interleaved_channels(reinterpret_cast<const uint8_t *>(invalid), strlen(invalid), &rtp,
                                                  &rtcp),
          "same RTP and RTCP channel is rejected");
}

} // namespace

int main()
{
    test_fragmented_request();
    test_interleaved_binary_and_concatenation();
    test_binary_zero_and_text_boundary();
    test_limits();
    test_transport_helpers();
    if (failures != 0) {
        fprintf(stderr, "%d RTSP framing test(s) failed\n", failures);
        return 1;
    }
    puts("RTSP framing tests passed");
    return 0;
}
