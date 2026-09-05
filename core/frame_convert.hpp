#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace baby_edge {
inline bool yuy2_to_rgb(const uint8_t *source, size_t bytes, uint32_t width, uint32_t height,
                        size_t stride, uint8_t *rgb, size_t capacity)
{
    if (!source || !rgb || !width || !height || width > 1920 || height > 1080 || (width & 1) ||
        stride < width * 2 || stride > bytes / height || capacity < static_cast<size_t>(width) * height * 3) return false;
    const auto clamp = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; column += 2) {
            const auto *pair = source + row * stride + column * 2;
            const int chroma_u = pair[1] - 128;
            const int chroma_v = pair[3] - 128;
            for (uint32_t pixel = 0; pixel < 2; ++pixel) {
                const int luma = std::max(0, static_cast<int>(pair[pixel * 2]) - 16);
                auto *output = rgb + (static_cast<size_t>(row) * width + column + pixel) * 3;
                output[0] = clamp((298 * luma + 409 * chroma_v + 128) >> 8);
                output[1] = clamp((298 * luma - 100 * chroma_u - 208 * chroma_v + 128) >> 8);
                output[2] = clamp((298 * luma + 516 * chroma_u + 128) >> 8);
            }
        }
    }
    return true;
}
}
