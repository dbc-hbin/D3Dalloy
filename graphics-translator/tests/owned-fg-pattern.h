#pragma once
#include <cstdint>
#include <vector>

// Caller-authored scene, not a reconstruction of the adapter: a translating
// checkerboard with an occluder and a per-frame red marker strip.
inline std::vector<std::uint16_t> fgScene(unsigned frame, unsigned width = 128, unsigned height = 128) {
    std::vector<std::uint16_t> pixels(width * height * 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const unsigned i = (y * width + x) * 4;
            const bool checker = (((x + frame * 4) / 16) ^ (y / 16)) & 1;
            pixels[i] = y < 8 ? std::uint16_t(0x3000 + frame * 0x80) : checker ? 0x3800 : 0x3400;
            pixels[i + 1] = x > 40 + frame * 2 && x < 64 + frame * 2 && y > 24 && y < 96 ? 0x3c00 : 0;
            pixels[i + 2] = checker ? 0 : 0x3800;
            pixels[i + 3] = 0x3c00;
        }
    return pixels;
}
