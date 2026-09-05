#include "frame_convert.hpp"
#include <array>
#include <cstdlib>
#include <iostream>

void require(bool condition)
{
    if (!condition) std::exit(1);
}

int main()
{
    const std::array<uint8_t, 12> source{16, 128, 235, 128, 99, 99, 16, 128, 235, 128, 99, 99};
    std::array<uint8_t, 12> rgb{};
    require(baby_edge::yuy2_to_rgb(source.data(), source.size(), 2, 2, 6, rgb.data(), rgb.size()));
    require(rgb == std::array<uint8_t, 12>{0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 255, 255});
    require(!baby_edge::yuy2_to_rgb(source.data(), 4, 2, 2, 6, rgb.data(), rgb.size()));
    require(!baby_edge::yuy2_to_rgb(source.data(), 12, 3, 2, 6, rgb.data(), rgb.size()));
    require(!baby_edge::yuy2_to_rgb(source.data(), 12, 2, 2, 3, rgb.data(), rgb.size()));
    require(!baby_edge::yuy2_to_rgb(source.data(), 12, 2, 2, 6, rgb.data(), 11));
    require(!baby_edge::yuy2_to_rgb(nullptr, 12, 2, 2, 6, rgb.data(), 12));
    std::cout << "YUY2 conversion tests passed\n";
}
