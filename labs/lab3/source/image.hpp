#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace lab3 {
struct ImageData {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
ImageData load_image(const std::filesystem::path& path);
} // namespace lab3
