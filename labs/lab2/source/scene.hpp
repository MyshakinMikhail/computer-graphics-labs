#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace lab2 {
struct Vertex { glm::vec3 position; glm::vec3 normal; };
struct Mesh { std::vector<Vertex> vertices; };
Mesh octahedron_mesh();
Mesh cube_mesh();
Mesh floor_mesh();
glm::vec3 safe_direction(glm::vec3 v, glm::vec3 fallback = {0, -1, 0});
float inverse_square(float distance_squared);
float cone_weight(float cosine, float inner_cosine, float outer_cosine);

struct Camera {
    glm::vec3 position{5.2f, 3.8f, 8.2f};
    float yaw = -123.0f, pitch = -19.0f, fov = 52.0f, speed = 4.0f;
    glm::vec3 forward() const;
    glm::mat4 view_projection(float aspect) const;
    void rotate(float dx, float dy);
    void move(glm::vec3 local, float dt, bool fast);
};
struct Material {
    glm::vec3 albedo{0.7f};
    glm::vec3 specular{0.7f};
    float shininess = 64.0f;
};
struct Object {
    glm::vec3 position{0}, rotation{0}, scale{1};
    Material material;
    bool floor = false;
    glm::mat4 model(float animation_angle = 0) const;
};
struct PointLight {
    glm::vec3 position{-2, 2, 1};
    glm::vec3 color{1.0f, 0.45f, 0.15f};
    float intensity = 10.0f;
    bool enabled = true;
};
struct SpotLight {
    glm::vec3 position{2, 3, 2}, direction{-0.4f, -1, -0.4f};
    glm::vec3 color{0.5f, 0.75f, 1.0f};
    float intensity = 25.0f, inner_angle = 18, outer_angle = 28;
    bool enabled = true;
};
// vec4 fields give identical std140/std430 offsets on C++ and GLSL sides.
struct alignas(16) SceneUniform {
    glm::mat4 view_projection;
    glm::vec4 camera_position, ambient, directional_direction, directional_color;
    glm::uvec4 light_counts;
};
struct alignas(16) ModelUniform {
    glm::mat4 model, normal_matrix;
    glm::vec4 albedo, specular_shininess;
};
struct alignas(16) PointGPU { glm::vec4 position, color_intensity; };
struct alignas(16) SpotGPU {
    glm::vec4 position, direction_inner, color_intensity, outer;
};
static_assert(sizeof(Vertex) == 24);
static_assert(sizeof(SceneUniform) == 144);
static_assert(sizeof(ModelUniform) == 160);
static_assert(sizeof(PointGPU) == 32 && sizeof(SpotGPU) == 64);

struct Scene {
    Camera camera;
    glm::vec3 ambient_color{0.65f, 0.75f, 1.0f};
    float ambient_intensity = 0.08f;
    glm::vec3 directional_direction{-0.4f, -1, -0.3f};
    glm::vec3 directional_color{1.0f, 0.94f, 0.82f};
    float directional_intensity = 0.45f;
    bool directional_enabled = true;
    bool show_markers = true, show_cones = true, show_floor = true;
    bool animate_objects = false, animate_lights = false;
    float animation_speed = 0.5f, angle = 0, light_angle = 0, cone_length = 3.0f;
    std::vector<Object> objects;
    std::vector<PointLight> points;
    std::vector<SpotLight> spots;
    void reset();
    SceneUniform uniform(float aspect) const;
    PointGPU point_gpu(size_t index) const;
    SpotGPU spot_gpu(size_t index) const;
    glm::vec3 point_position(size_t index) const;
};
} // namespace lab2
