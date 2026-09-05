#pragma once
#include "dl_model_base.hpp"
#include "yolo26.hpp"
#include <cstddef>
#include <cstdint>

namespace baby_edge {
struct RgbFrame {
    uint8_t *data;
    size_t bytes;
    uint16_t width;
    uint16_t height;
    int64_t captured_ms;
};

struct Detection {
    bool valid = false;
    float person_score = 0;
    int64_t inference_us = 0;
};

class PersonDetector {
public:
    explicit PersonDetector(const uint8_t *model_data);
    Detection detect(const RgbFrame &frame);
private:
    bool compatible();
    dl::Model model_;
    YOLO26 processor_;
};
}
