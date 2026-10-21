#include "scene.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace lab2 {
namespace {
void triangle(Mesh& m, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    const glm::vec3 normal = safe_direction(glm::cross(b - a, c - a));
    m.vertices.insert(m.vertices.end(), {{a, normal}, {b, normal}, {c, normal}});
}
}
glm::vec3 safe_direction(glm::vec3 v, glm::vec3 fallback) {
    const float length_squared = glm::dot(v, v);
    return length_squared > 1e-10f ? v / std::sqrt(length_squared) : fallback;
}
float inverse_square(float distance_squared) {
    return 1.0f / std::max(distance_squared, 0.01f);
}
float cone_weight(float cosine, float inner_cosine, float outer_cosine) {
    if (inner_cosine - outer_cosine < 1e-6f)
        return cosine >= outer_cosine ? 1.0f : 0.0f;
    return std::clamp((cosine - outer_cosine) / (inner_cosine - outer_cosine), 0.0f, 1.0f);
}
Mesh octahedron_mesh() {
    const std::array<glm::vec3, 6> p{{{0,1,0},{0,-1,0},{1,0,0},{0,0,1},{-1,0,0},{0,0,-1}}};
    const std::array<int, 24> indices{{0,3,2, 0,4,3, 0,5,4, 0,2,5, 1,2,3, 1,3,4, 1,4,5, 1,5,2}};
    Mesh m;
    for (size_t i = 0; i < indices.size(); i += 3)
        triangle(m, p[indices[i]], p[indices[i+1]], p[indices[i+2]]);
    return m;
}
Mesh cube_mesh() {
    const std::array<glm::vec3, 8> p{{{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                    {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}};
    const std::array<int, 36> indices{{0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,
                                     3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5}};
    Mesh m;
    for (size_t i = 0; i < indices.size(); i += 3)
        triangle(m, p[indices[i]], p[indices[i+1]], p[indices[i+2]]);
    return m;
}
Mesh floor_mesh() {
    Mesh m;
    triangle(m, {-7,0,-7}, {-7,0,7}, {7,0,7});
    triangle(m, {-7,0,-7}, {7,0,7}, {7,0,-7});
    return m;
}
glm::vec3 Camera::forward() const {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return {std::cos(y)*std::cos(p), std::sin(p), std::sin(y)*std::cos(p)};
}
glm::mat4 Camera::view_projection(float aspect) const {
    auto projection = glm::perspective(glm::radians(std::clamp(fov, 20.0f, 100.0f)),
                                       std::max(aspect, 0.01f), 0.05f, 150.0f);
    projection[1][1] *= -1.0f;
    return projection * glm::lookAt(position, position + forward(), glm::vec3(0,1,0));
}
void Camera::rotate(float dx, float dy) {
    yaw = std::remainder(yaw + dx, 360.0f);
    pitch = std::clamp(pitch - dy, -89.0f, 89.0f);
}
void Camera::move(glm::vec3 local, float dt, bool fast) {
    if (glm::dot(local, local) < 1e-10f) return;
    const glm::vec3 right = safe_direction(glm::cross(forward(), {0,1,0}), {1,0,0});
    const glm::vec3 movement = right*local.x + glm::vec3(0,1,0)*local.y + forward()*local.z;
    position += safe_direction(movement) * speed * std::clamp(dt, 0.0f, 0.1f) * (fast ? 3.0f : 1.0f);
}
glm::mat4 Object::model(float animation_angle) const {
    glm::mat4 m = glm::translate(glm::mat4(1), position);
    m = glm::rotate(m, glm::radians(rotation.z), {0,0,1});
    m = glm::rotate(m, glm::radians(rotation.y) + (floor ? 0 : animation_angle), {0,1,0});
    m = glm::rotate(m, glm::radians(rotation.x), {1,0,0});
    return glm::scale(m, glm::max(scale, glm::vec3(0.05f)));
}
void Scene::reset() {
    *this = Scene{};
    objects = {
        {{-1.5f,0,0},{12,20,0},{1,1,1},{{0.85f,0.45f,0.18f},{0.95f,0.8f,0.6f},96},false},
        {{1.5f,0,0},{-12,-25,0},{1,1,1},{{0.15f,0.52f,0.8f},{0.8f,0.95f,1},32},false},
        {{0,-1.2f,0},{0,0,0},{1,1,1},{{0.23f,0.26f,0.32f},{0.2f,0.2f,0.2f},16},true}};
    points = {{{-2.8f,1.8f,1.5f},{1,0.3f,0.08f},12,true},
              {{2.8f,1.6f,-1.5f},{0.12f,0.45f,1},14,true}};
    spots = {{{0,3.2f,2},{0,-1,-0.55f},{0.7f,1,0.8f},28,16,27,true}};
}
glm::vec3 Scene::point_position(size_t index) const {
    const glm::vec3 p = points[index].position;
    const float c = std::cos(light_angle), s = std::sin(light_angle);
    return {c*p.x+s*p.z,p.y,-s*p.x+c*p.z};
}
SceneUniform Scene::uniform(float aspect) const {
    return {camera.view_projection(aspect), glm::vec4(camera.position,1),
        glm::vec4(ambient_color, ambient_intensity),
        glm::vec4(safe_direction(directional_direction),0),
        glm::vec4(directional_color, directional_enabled ? directional_intensity : 0),
        glm::uvec4(uint32_t(points.size()),uint32_t(spots.size()),0,0)};
}
PointGPU Scene::point_gpu(size_t index) const {
    const auto& p = points[index];
    return {glm::vec4(point_position(index),1), glm::vec4(p.color,p.enabled ? p.intensity : 0)};
}
SpotGPU Scene::spot_gpu(size_t index) const {
    const auto& s = spots[index];
    const float inner = std::clamp(s.inner_angle,0.5f,85.0f);
    const float outer = std::clamp(s.outer_angle,inner,85.0f);
    return {glm::vec4(s.position,1),
        glm::vec4(safe_direction(s.direction),std::cos(glm::radians(inner))),
        glm::vec4(s.color,s.enabled ? s.intensity : 0),
        glm::vec4(std::cos(glm::radians(outer)),0,0,0)};
}
} // namespace lab2
