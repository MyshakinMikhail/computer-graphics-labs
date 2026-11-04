#pragma once
#include "scene.hpp"
#include "graphics_internal.hpp"
namespace lab3::renderer {
bool initialize(const Scene& scene);
void shutdown();
void render(const graphics::internal::FrameData& frame, const Scene& scene);
size_t point_capacity();
size_t spot_capacity();
size_t light_limit(bool spot);
} // namespace lab3::renderer
