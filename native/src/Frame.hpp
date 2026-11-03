#pragma once
#include <cstdint>
#include <vector>

enum class PixelFormat {
    BGR,
};

struct Frame {
    int width = 640;
    int height = 480;
    std::vector<uint8_t> pixels; // BGR layout, 3 bytes per pixel

    Frame() = default;
    Frame(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w * h * 3), 0) {}
    Frame(const Frame&) = default;
    Frame& operator=(const Frame&) = default;

    uint8_t* ptr(size_t x, size_t y) { return pixels.data() + (y * width + x) * 3; }
    const uint8_t* ptr(size_t x, size_t y) const { return pixels.data() + (y * width + x) * 3; }
};
