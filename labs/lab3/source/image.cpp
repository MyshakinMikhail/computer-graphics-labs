#include "image.hpp"
#include <limits>
#include <memory>
#include <stdexcept>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

namespace lab3 {
ImageData load_image(const std::filesystem::path& path) {
    ImageData image;
    int channels=0;
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(
        stbi_load(path.string().c_str(),&image.width,&image.height,&channels,STBI_rgb_alpha),
        &stbi_image_free);
    if (!pixels) throw std::runtime_error("Cannot decode texture "+path.string()+": "+
        (stbi_failure_reason()?stbi_failure_reason():"unknown error"));
    if (image.width<=0 || image.height<=0 ||
        size_t(image.width)>std::numeric_limits<size_t>::max()/4/size_t(image.height))
        throw std::runtime_error("Invalid texture dimensions: "+path.string());
    image.rgba.assign(pixels.get(),pixels.get()+size_t(image.width)*size_t(image.height)*4);
    return image;
}
} // namespace lab3
