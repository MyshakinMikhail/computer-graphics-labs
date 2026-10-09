#include "application.hpp"

#include <imgui.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace application {
namespace {

constexpr float pi = 3.14159265358979323846f;
constexpr uint32_t object_count = 2;
constexpr std::array<glm::vec3, 6> positions{{
    {0, 1, 0}, {0, -1, 0}, {1, 0, 0},
    {0, 0, 1}, {-1, 0, 0}, {0, 0, -1},
}};
// Eight outward-facing equilateral triangles.
constexpr std::array<uint16_t, 24> indices{{
    0, 3, 2,  0, 4, 3,  0, 5, 4,  0, 2, 5,
    1, 2, 3,  1, 3, 4,  1, 4, 5,  1, 5, 2,
}};

struct Vertex { glm::vec3 position; glm::vec3 color; glm::vec3 normal; };
static_assert(sizeof(Vertex) == 36);
struct ObjectUniform { glm::mat4 mvp; glm::mat4 model; glm::vec4 tint; };
static_assert(sizeof(ObjectUniform) == 144);
struct Object {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // Degrees in the UI.
    glm::vec3 scale{0.8f};
    glm::vec3 tint{1.0f};
    VkBuffer uniform_buffer = VK_NULL_HANDLE;
    VmaAllocation uniform_allocation = VK_NULL_HANDLE;
    void* uniform_mapping = nullptr;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

std::array<Object, object_count> objects;
VkBuffer vertex_buffer = VK_NULL_HANDLE, index_buffer = VK_NULL_HANDLE;
VmaAllocation vertex_allocation = VK_NULL_HANDLE, index_allocation = VK_NULL_HANDLE;
VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;

bool use_perspective = true;
bool animation_playing = true;
float animation_speed = 1.0f;
float orbit_radius = 0.35f;
float orbit_height = 0.25f;
float orbit_angle = 0.0f;
double previous_time = -1.0;

bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                   VmaAllocation& allocation, void*& mapping) {
    auto& ctx = graphics::internal::context;
    const VkBufferCreateInfo buffer_info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const VmaAllocationCreateInfo allocation_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(ctx.allocator, &buffer_info, &allocation_info,
                        &buffer, &allocation, &result) != VK_SUCCESS) {
        std::cerr << "Failed to allocate Vulkan buffer\n";
        return false;
    }
    mapping = result.pMappedData;
    return true;
}

VkShaderModule load_shader(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { std::cerr << "Cannot open shader " << path << '\n'; return VK_NULL_HANDLE; }
    const auto length = file.tellg();
    if (length <= 0 || length % 4 != 0) return VK_NULL_HANDLE;
    std::vector<uint32_t> code(static_cast<size_t>(length) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), length);
    if (!file) return VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(length), .pCode = code.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module) != VK_SUCCESS)
        std::cerr << "Cannot create shader module " << path << '\n';
    return module;
}

bool create_descriptors() {
    auto& ctx = graphics::internal::context;
    const VkDescriptorSetLayoutBinding binding{.binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    const VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    if (vkCreateDescriptorSetLayout(ctx.device, &layout_info, nullptr, &descriptor_layout) != VK_SUCCESS)
        return false;
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, object_count};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = object_count, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    if (vkCreateDescriptorPool(ctx.device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS)
        return false;
    std::array<VkDescriptorSetLayout, object_count> layouts{};
    layouts.fill(descriptor_layout);
    std::array<VkDescriptorSet, object_count> sets{};
    const VkDescriptorSetAllocateInfo alloc_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = object_count,
        .pSetLayouts = layouts.data()};
    if (vkAllocateDescriptorSets(ctx.device, &alloc_info, sets.data()) != VK_SUCCESS)
        return false;
    for (uint32_t i = 0; i < object_count; ++i) {
        Object& object = objects[i];
        object.descriptor_set = sets[i];
        if (!create_buffer(sizeof(ObjectUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           object.uniform_buffer, object.uniform_allocation, object.uniform_mapping))
            return false;
        const VkDescriptorBufferInfo buffer_info{object.uniform_buffer, 0, sizeof(ObjectUniform)};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = sets[i], .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffer_info};
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
    }
    return true;
}

bool create_pipeline() {
    auto& ctx = graphics::internal::context;
    const VkShaderModule vert = load_shader("shaders/octahedron.vert.spv");
    const VkShaderModule frag = load_shader("shaders/octahedron.frag.spv");
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(ctx.device, vert, nullptr);
        if (frag) vkDestroyShaderModule(ctx.device, frag, nullptr);
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"},
    };
    const VkVertexInputBindingDescription vertex_binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS};
    const VkPipelineColorBlendAttachmentState color_attachment{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo color_blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color_attachment};
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = states};
    const VkPipelineLayoutCreateInfo layout_info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &descriptor_layout};
    bool ok = vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &pipeline_layout) == VK_SUCCESS;
    if (ok) {
        const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
            .pViewportState = &viewport, .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample, .pDepthStencilState = &depth,
            .pColorBlendState = &color_blend, .pDynamicState = &dynamic,
            .layout = pipeline_layout, .renderPass = ctx.render_pass, .subpass = 0};
        ok = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info,
                                       nullptr, &pipeline) == VK_SUCCESS;
    }
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    if (!ok) std::cerr << "Failed to create octahedron pipeline\n";
    return ok;
}

void write_uniforms() {
    auto& ctx = graphics::internal::context;
    const float aspect = float(ctx.swapchain_extent.width) /
                         float(std::max(1u, ctx.swapchain_extent.height));
    glm::mat4 projection = use_perspective
        ? glm::perspective(glm::radians(55.0f), aspect, 0.1f, 100.0f)
        : glm::ortho(-2.8f * aspect, 2.8f * aspect, -2.8f, 2.8f, 0.1f, 100.0f);
    projection[1][1] *= -1.0f; // Vulkan's framebuffer Y axis points down.
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 7), glm::vec3(0), glm::vec3(0, 1, 0));
    for (uint32_t i = 0; i < object_count; ++i) {
        const Object& object = objects[i];
        const float phase = orbit_angle + (i == 0 ? 0.0f : pi);
        const glm::vec3 animated_position = object.position + glm::vec3(
            orbit_radius * std::cos(phase), orbit_height * std::sin(2.0f * phase),
            orbit_radius * std::sin(phase));
        glm::mat4 model = glm::translate(glm::mat4(1), animated_position);
        model = glm::rotate(model, glm::radians(object.rotation.z), glm::vec3(0, 0, 1));
        model = glm::rotate(model, glm::radians(object.rotation.y) + orbit_angle, glm::vec3(0, 1, 0));
        model = glm::rotate(model, glm::radians(object.rotation.x), glm::vec3(1, 0, 0));
        model = glm::scale(model, object.scale);
        const ObjectUniform uniform{projection * view * model, model, glm::vec4(object.tint, 1)};
        std::memcpy(object.uniform_mapping, &uniform, sizeof(uniform));
        vmaFlushAllocation(ctx.allocator, object.uniform_allocation, 0, VK_WHOLE_SIZE);
    }
}

} // namespace

bool initialize() {
    auto& ctx = graphics::internal::context;
    objects[0].position = {-1.15f, 0, 0};
    objects[1].position = { 1.15f, 0, 0};
    objects[0].rotation = {25.0f, 25.0f, 0.0f};
    objects[1].rotation = {-20.0f, -30.0f, 0.0f};
    objects[0].tint = {1.0f, 0.75f, 0.65f};
    objects[1].tint = {0.55f, 0.80f, 1.0f};
    // Faces share six geometric corners; GPU vertices are duplicated for flat face normals.
    std::array<Vertex, indices.size()> vertices{};
    std::array<uint16_t, indices.size()> draw_indices{};
    for (size_t face = 0; face < indices.size() / 3; ++face) {
        const glm::vec3 a = positions[indices[face * 3]];
        const glm::vec3 b = positions[indices[face * 3 + 1]];
        const glm::vec3 c = positions[indices[face * 3 + 2]];
        const glm::vec3 normal = glm::normalize(glm::cross(b - a, c - a));
        for (size_t corner = 0; corner < 3; ++corner) {
            const size_t at = face * 3 + corner;
            const glm::vec3 position = positions[indices[at]];
            vertices[at] = {position, 0.35f + 0.325f * (position + glm::vec3(1)), normal};
            draw_indices[at] = uint16_t(at);
        }
    }
    void* mapping = nullptr;
    bool ok = create_buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            vertex_buffer, vertex_allocation, mapping);
    if (ok) {
        std::memcpy(mapping, vertices.data(), sizeof(vertices));
        vmaFlushAllocation(ctx.allocator, vertex_allocation, 0, VK_WHOLE_SIZE);
        ok = create_buffer(sizeof(draw_indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           index_buffer, index_allocation, mapping);
    }
    if (ok) {
        std::memcpy(mapping, draw_indices.data(), sizeof(draw_indices));
        vmaFlushAllocation(ctx.allocator, index_allocation, 0, VK_WHOLE_SIZE);
        ok = create_descriptors() && create_pipeline();
    }
    if (!ok) shutdown();
    return ok;
}

void shutdown() {
    auto& ctx = graphics::internal::context;
    vkQueueWaitIdle(ctx.graphics_queue);
    if (pipeline) vkDestroyPipeline(ctx.device, pipeline, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(ctx.device, descriptor_pool, nullptr);
    if (descriptor_layout) vkDestroyDescriptorSetLayout(ctx.device, descriptor_layout, nullptr);
    for (Object& object : objects)
        if (object.uniform_buffer)
            vmaDestroyBuffer(ctx.allocator, object.uniform_buffer, object.uniform_allocation);
    if (index_buffer) vmaDestroyBuffer(ctx.allocator, index_buffer, index_allocation);
    if (vertex_buffer) vmaDestroyBuffer(ctx.allocator, vertex_buffer, vertex_allocation);
}

void update(double time) {
    if (previous_time >= 0.0 && animation_playing) {
        const double dt = std::clamp(time - previous_time, 0.0, 0.1);
        orbit_angle = std::fmod(orbit_angle + float(dt) * animation_speed, 2.0f * pi);
    }
    previous_time = time;
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 440), ImGuiCond_FirstUseEver);
    ImGui::Begin("Lab 1 - Regular octahedra");
    ImGui::Text("Variant 2 | 6 vertices | 8 faces");
    ImGui::TextDisabled("Each object has its own descriptor set");
    ImGui::Separator();
    ImGui::Checkbox("Perspective projection", &use_perspective);
    ImGui::Checkbox("Play animation", &animation_playing);
    ImGui::SliderFloat("Speed", &animation_speed, 0.0f, 4.0f);
    ImGui::SliderFloat("Orbit radius", &orbit_radius, 0.0f, 1.0f);
    ImGui::SliderFloat("Orbit height", &orbit_height, 0.0f, 1.0f);
    if (ImGui::BeginTabBar("Objects")) {
        for (uint32_t i = 0; i < object_count; ++i) {
            ImGui::PushID(int(i));
            if (ImGui::BeginTabItem(i == 0 ? "Octahedron 1" : "Octahedron 2")) {
                Object& object = objects[i];
                ImGui::TextUnformatted("Position");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat3("##position", glm::value_ptr(object.position), 0.02f, -5.0f, 5.0f);
                ImGui::TextUnformatted("Rotation (degrees)");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat3("##rotation", glm::value_ptr(object.rotation), 0.5f, -360.0f, 360.0f);
                ImGui::TextUnformatted("Scale");
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat3("##scale", glm::value_ptr(object.scale), 0.01f, 0.1f, 3.0f);
                ImGui::TextUnformatted("Color multiplier");
                ImGui::SetNextItemWidth(-1);
                ImGui::ColorEdit3("##tint", glm::value_ptr(object.tint));
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
    if (!fd.command_buffer || !fd.framebuffer) return;
    auto& ctx = graphics::internal::context;
    write_uniforms(); // prepare() already waited for the previous frame's fence.
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(fd.command_buffer, 0);
    vkBeginCommandBuffer(fd.command_buffer, &begin);
    const VkClearValue clear[] = {
        {.color = {{0.035f, 0.055f, 0.085f, 1.0f}}},
        {.depthStencil = {1.0f, 0}},
    };
    const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = ctx.render_pass, .framebuffer = fd.framebuffer,
        .renderArea = {{0, 0}, ctx.swapchain_extent}, .clearValueCount = 2, .pClearValues = clear};
    vkCmdBeginRenderPass(fd.command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(ctx.swapchain_extent.width),
        float(ctx.swapchain_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, ctx.swapchain_extent};
    vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer, &offset);
    vkCmdBindIndexBuffer(fd.command_buffer, index_buffer, 0, VK_INDEX_TYPE_UINT16);
    for (const Object& object : objects) {
        vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0, 1, &object.descriptor_set, 0, nullptr);
        vkCmdDrawIndexed(fd.command_buffer, uint32_t(indices.size()), 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(fd.command_buffer);
    vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
