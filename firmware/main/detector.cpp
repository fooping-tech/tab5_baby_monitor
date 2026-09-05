#include "detector.hpp"
#include <algorithm>
#include <cmath>
#include "esp_timer.h"

namespace baby_edge {
PersonDetector::PersonDetector(const uint8_t *model_data) :
    model_(reinterpret_cast<const char *>(model_data), fbs::MODEL_LOCATION_IN_FLASH_RODATA,
           0, dl::MEMORY_MANAGER_GREEDY, nullptr, true),
    processor_(&model_, 32, 0.25f)
{}

bool PersonDetector::compatible()
{
    const auto &inputs = model_.get_inputs();
    if (inputs.size() != 1 || !inputs.begin()->second ||
        inputs.begin()->second->shape != std::vector<int>({1, 512, 512, 3})) return false;
    const auto &outputs = model_.get_outputs();
    int grid = 64;
    for (const auto *scale : {"p3", "p4", "p5"}) {
        const std::string prefix = std::string("one2one_") + scale;
        const auto box = outputs.find(prefix + "_box");
        const auto cls = outputs.find(prefix + "_cls");
        if (box == outputs.end() || cls == outputs.end() || !box->second || !cls->second) return false;
        const auto *boxes = box->second;
        const auto *classes = cls->second;
        if (boxes->shape != std::vector<int>({1, grid, grid, 4}) ||
            classes->shape != std::vector<int>({1, grid, grid, 80}) ||
            boxes->dtype != classes->dtype ||
            (boxes->dtype != dl::DATA_TYPE_INT8 && boxes->dtype != dl::DATA_TYPE_INT16)) return false;
        grid /= 2;
    }
    return true;
}

Detection PersonDetector::detect(const RgbFrame &frame)
{
    if (!frame.data || frame.width == 0 || frame.height == 0 ||
        frame.bytes != static_cast<size_t>(frame.width) * frame.height * 3 || !compatible()) return {};
    dl::image::img_t image = {frame.data, frame.width, frame.height, dl::image::DL_IMAGE_PIX_TYPE_RGB888};
    const auto start = esp_timer_get_time();
    processor_.preprocess(image);
    model_.run();
    const auto results = processor_.postprocess(model_.get_outputs());
    Detection detection{true, 0, esp_timer_get_time() - start};
    for (const auto &result : results) {
        if (!std::isfinite(result.score) || result.score < 0 || result.score > 1) return {};
        if (result.category == 0) detection.person_score = std::max(detection.person_score, result.score);
    }
    return detection;
}
}
