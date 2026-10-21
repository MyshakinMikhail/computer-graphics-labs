#include "renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace lab2::renderer {
namespace {
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapping = nullptr;
    VkDeviceSize size = 0;
};
struct Geometry { Buffer buffer; uint32_t count = 0; };
struct ModelData { Buffer uniform; VkDescriptorSet descriptor = VK_NULL_HANDLE; };
struct MarkerPush { glm::mat4 mvp; glm::vec4 color; };
static_assert(sizeof(MarkerPush) == 80);
Buffer scene_buffer, point_buffer, spot_buffer, line_buffer;
Geometry octahedron, cube, floor;
std::vector<ModelData> models;
VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE, model_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet scene_set = VK_NULL_HANDLE;
VkPipelineLayout lit_layout = VK_NULL_HANDLE, unlit_layout = VK_NULL_HANDLE;
VkPipeline lit_pipeline = VK_NULL_HANDLE, marker_pipeline = VK_NULL_HANDLE, line_pipeline = VK_NULL_HANDLE;
size_t points_capacity = 0, spots_capacity = 0;
VkDeviceSize max_storage_range = 0;

void destroy_buffer(Buffer& buffer) {
    if (buffer.handle) vmaDestroyBuffer(graphics::internal::context.allocator, buffer.handle, buffer.allocation);
    buffer = {};
}
bool create_buffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
    const auto& ctx = graphics::internal::context;
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const VmaAllocationCreateInfo alloc{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT, .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(ctx.allocator, &info, &alloc, &buffer.handle, &buffer.allocation, &result) != VK_SUCCESS)
        return false;
    buffer.mapping = result.pMappedData;
    buffer.size = size;
    return true;
}
void upload(const Buffer& buffer, const void* data, size_t bytes) {
    if (bytes == 0) return;
    if (bytes > buffer.size) throw std::runtime_error("GPU buffer upload exceeds allocation");
    std::memcpy(buffer.mapping, data, bytes);
    if (vmaFlushAllocation(graphics::internal::context.allocator, buffer.allocation, 0, bytes) != VK_SUCCESS)
        throw std::runtime_error("Cannot flush GPU buffer");
}
void bind_buffer(VkDescriptorSet set, uint32_t binding, const Buffer& buffer, VkDescriptorType type) {
    const VkDescriptorBufferInfo info{buffer.handle, 0, buffer.size};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .dstBinding = binding, .descriptorCount = 1,
        .descriptorType = type, .pBufferInfo = &info};
    vkUpdateDescriptorSets(graphics::internal::context.device, 1, &write, 0, nullptr);
}
bool ensure_storage(Buffer& buffer, size_t& capacity, size_t count, size_t stride, uint32_t binding) {
    const size_t limit = size_t(max_storage_range / stride);
    if (count > limit) throw std::runtime_error("Storage buffer exceeds device limit");
    count = std::max(size_t(1), count); // Vulkan descriptors must reference a nonempty buffer, even at count=0.
    if (capacity >= count) return true;
    const size_t next = std::min(limit, std::max(count, std::max(size_t(4), capacity * 2)));
    Buffer replacement;
    if (!create_buffer(replacement, next * stride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
    // prepare() has waited for the sole in-flight fence. No GPU command can use the old buffer now.
    bind_buffer(scene_set, binding, replacement, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    destroy_buffer(buffer);
    buffer = replacement;
    capacity = next;
    return true;
}
bool create_geometry(Geometry& geometry, const Mesh& mesh) {
    geometry.count = uint32_t(mesh.vertices.size());
    if (!create_buffer(geometry.buffer, mesh.vertices.size()*sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return false;
    upload(geometry.buffer, mesh.vertices.data(), mesh.vertices.size()*sizeof(Vertex));
    return true;
}
VkShaderModule load_shader(const char* name) {
    const std::string path = std::string(LAB_SHADER_DIR) + "/" + name + ".spv";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open shader: " + path);
    const auto length = file.tellg();
    if (length <= 0 || length % 4 != 0) throw std::runtime_error("Invalid SPIR-V: " + path);
    std::vector<uint32_t> code(size_t(length) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), length);
    if (!file) throw std::runtime_error("Cannot read shader: " + path);
    const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size_t(length), .pCode = code.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("Cannot create shader: " + path);
    return module;
}
bool create_descriptors(size_t object_count) {
    const auto device = graphics::internal::context.device;
    const VkDescriptorSetLayoutBinding scene_bindings[] = {
        {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo scene_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = scene_bindings};
    if (vkCreateDescriptorSetLayout(device, &scene_info, nullptr, &scene_layout) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding model_binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,
        VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    const VkDescriptorSetLayoutCreateInfo model_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &model_binding};
    if (vkCreateDescriptorSetLayout(device, &model_info, nullptr, &model_layout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,uint32_t(object_count+1)},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    const VkDescriptorPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = uint32_t(object_count+1), .poolSizeCount = 2, .pPoolSizes = sizes};
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayout> layouts(object_count+1,model_layout);
    layouts[0] = scene_layout;
    std::vector<VkDescriptorSet> sets(object_count+1);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = uint32_t(layouts.size()), .pSetLayouts = layouts.data()};
    if (vkAllocateDescriptorSets(device, &allocate, sets.data()) != VK_SUCCESS) return false;
    scene_set = sets[0];
    if (!create_buffer(scene_buffer,sizeof(SceneUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    bind_buffer(scene_set,0,scene_buffer,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    models.resize(object_count);
    for (size_t i = 0; i < object_count; ++i) {
        models[i].descriptor = sets[i+1];
        if (!create_buffer(models[i].uniform,sizeof(ModelUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
        bind_buffer(models[i].descriptor,0,models[i].uniform,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    }
    return true;
}
bool create_pipeline(VkPipeline& pipeline, VkPipelineLayout layout, bool lit, bool lines) {
    const auto& ctx = graphics::internal::context;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    try {
        vert = load_shader(lit ? "lit.vert" : "unlit.vert");
        frag = load_shader(lit ? "lit.frag" : "unlit.frag");
    } catch (...) {
        if (vert) vkDestroyShaderModule(ctx.device,vert,nullptr);
        throw;
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=frag,.pName="main"}};
    const VkVertexInputBindingDescription binding{0,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)}};
    const VkPipelineVertexInputStateCreateInfo vertex{.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=lit?2u:1u,.pVertexAttributeDescriptions=attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly{.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=lines?VK_PRIMITIVE_TOPOLOGY_LINE_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    const VkPipelineRasterizationStateCreateInfo raster{.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    const VkPipelineMultisampleStateCreateInfo samples{.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth{.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=lines?VK_FALSE:VK_TRUE,.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL};
    const VkPipelineColorBlendAttachmentState attachment{.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend{.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment};
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=states};
    const VkGraphicsPipelineCreateInfo info{.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vertex,.pInputAssemblyState=&assembly,
        .pViewportState=&viewport,.pRasterizationState=&raster,.pMultisampleState=&samples,
        .pDepthStencilState=&depth,.pColorBlendState=&blend,.pDynamicState=&dynamic,
        .layout=layout,.renderPass=ctx.render_pass,.subpass=0};
    const bool ok = vkCreateGraphicsPipelines(ctx.device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline)==VK_SUCCESS;
    vkDestroyShaderModule(ctx.device,vert,nullptr);
    vkDestroyShaderModule(ctx.device,frag,nullptr);
    return ok;
}
void bind_geometry(VkCommandBuffer command, const Geometry& geometry) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command,0,1,&geometry.buffer.handle,&offset);
}
void marker(VkCommandBuffer command, glm::mat4 vp, glm::vec3 position, glm::vec3 color) {
    const auto model = glm::scale(glm::translate(glm::mat4(1),position),glm::vec3(0.09f));
    const MarkerPush push{vp*model,glm::vec4(color,1)};
    vkCmdPushConstants(command,unlit_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDraw(command,cube.count,1,0,0);
}
void line(std::vector<Vertex>& vertices, glm::vec3 a, glm::vec3 b) {
    vertices.push_back({a,{0,0,0}}); vertices.push_back({b,{0,0,0}});
}
void cone(std::vector<Vertex>& vertices, const SpotLight& light, float length, float degrees) {
    const auto direction = safe_direction(light.direction);
    const auto tangent = safe_direction(glm::cross(direction,std::abs(direction.y)<0.95f?glm::vec3(0,1,0):glm::vec3(1,0,0)));
    const auto bitangent = glm::cross(direction,tangent);
    const glm::vec3 center = light.position+length*direction;
    const float radius = length*std::tan(glm::radians(degrees));
    constexpr int segments = 48;
    for (int i=0;i<segments;++i) {
        const float a=2*3.14159265359f*float(i)/segments, b=2*3.14159265359f*float(i+1)/segments;
        const auto p=center+radius*(std::cos(a)*tangent+std::sin(a)*bitangent);
        const auto q=center+radius*(std::cos(b)*tangent+std::sin(b)*bitangent);
        line(vertices,p,q);
        if(i%12==0) line(vertices,light.position,p);
    }
}
} // namespace

bool initialize(const Scene& scene) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device,&properties);
    max_storage_range=properties.limits.maxStorageBufferRange;
    bool ok = create_geometry(octahedron,octahedron_mesh()) && create_geometry(cube,cube_mesh()) &&
        create_geometry(floor,floor_mesh()) && create_descriptors(scene.objects.size());
    if(ok) ok=ensure_storage(point_buffer,points_capacity,scene.points.size(),sizeof(PointGPU),1) &&
              ensure_storage(spot_buffer,spots_capacity,scene.spots.size(),sizeof(SpotGPU),2);
    const VkDescriptorSetLayout layouts[]{scene_layout,model_layout};
    const VkPipelineLayoutCreateInfo lit_info{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=2,.pSetLayouts=layouts};
    const auto device=graphics::internal::context.device;
    if(ok) ok=vkCreatePipelineLayout(device,&lit_info,nullptr,&lit_layout)==VK_SUCCESS;
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(MarkerPush)};
    const VkPipelineLayoutCreateInfo unlit_info{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&range};
    if(ok) ok=vkCreatePipelineLayout(device,&unlit_info,nullptr,&unlit_layout)==VK_SUCCESS;
    if(ok) ok=create_pipeline(lit_pipeline,lit_layout,true,false) && create_pipeline(marker_pipeline,unlit_layout,false,false) &&
              create_pipeline(line_pipeline,unlit_layout,false,true);
    return ok;
}
void shutdown() {
    const auto& ctx=graphics::internal::context;
    vkDeviceWaitIdle(ctx.device);
    for(auto p:{lit_pipeline,marker_pipeline,line_pipeline}) if(p) vkDestroyPipeline(ctx.device,p,nullptr);
    for(auto p:{lit_layout,unlit_layout}) if(p) vkDestroyPipelineLayout(ctx.device,p,nullptr);
    if(descriptor_pool) vkDestroyDescriptorPool(ctx.device,descriptor_pool,nullptr);
    for(auto p:{scene_layout,model_layout}) if(p) vkDestroyDescriptorSetLayout(ctx.device,p,nullptr);
    for(auto& model:models) destroy_buffer(model.uniform);
    models.clear();
    for(auto* b:{&scene_buffer,&point_buffer,&spot_buffer,&line_buffer,&octahedron.buffer,&cube.buffer,&floor.buffer}) destroy_buffer(*b);
}
void render(const graphics::internal::FrameData& frame,const Scene& scene) {
    if(!frame.command_buffer||!frame.framebuffer) return;
    if(!ensure_storage(point_buffer,points_capacity,scene.points.size(),sizeof(PointGPU),1) ||
       !ensure_storage(spot_buffer,spots_capacity,scene.spots.size(),sizeof(SpotGPU),2))
        throw std::runtime_error("Cannot grow light buffers");
    const auto& ctx=graphics::internal::context;
    const auto uniform=scene.uniform(float(ctx.swapchain_extent.width)/float(std::max(1u,ctx.swapchain_extent.height)));
    upload(scene_buffer,&uniform,sizeof(uniform));
    std::vector<PointGPU> points; for(size_t i=0;i<scene.points.size();++i) points.push_back(scene.point_gpu(i));
    std::vector<SpotGPU> spots; for(size_t i=0;i<scene.spots.size();++i) spots.push_back(scene.spot_gpu(i));
    upload(point_buffer,points.data(),points.size()*sizeof(PointGPU));
    upload(spot_buffer,spots.data(),spots.size()*sizeof(SpotGPU));
    for(size_t i=0;i<scene.objects.size();++i) {
        const auto& object=scene.objects[i];
        const auto m=object.model(scene.angle);
        const ModelUniform data{m,glm::transpose(glm::inverse(m)),glm::vec4(object.material.albedo,1),
            glm::vec4(object.material.specular,std::max(1.0f,object.material.shininess))};
        upload(models[i].uniform,&data,sizeof(data));
    }
    std::vector<Vertex> lines;
    if(scene.show_cones) for(const auto& s:scene.spots) {
        cone(lines,s,scene.cone_length,std::clamp(s.outer_angle,s.inner_angle,85.0f));
        cone(lines,s,scene.cone_length,std::clamp(s.inner_angle,0.5f,85.0f));
        line(lines,s.position,s.position+safe_direction(s.direction)*scene.cone_length);
    }
    if(!lines.empty()) {
        const size_t bytes=lines.size()*sizeof(Vertex);
        if(line_buffer.size<bytes) {
            Buffer replacement;
            if(!create_buffer(replacement,std::max(VkDeviceSize(bytes),line_buffer.size*2),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
                throw std::runtime_error("Cannot grow cone geometry buffer");
            destroy_buffer(line_buffer); line_buffer=replacement;
        }
        upload(line_buffer,lines.data(),bytes);
    }
    const auto command=frame.command_buffer;
    if(vkResetCommandBuffer(command,0)!=VK_SUCCESS) throw std::runtime_error("Cannot reset frame commands");
    const VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS) throw std::runtime_error("Cannot begin frame");
    const VkClearValue clear[]{{.color={{0.018f,0.025f,0.045f,1}}},{.depthStencil={1,0}}};
    const VkRenderPassBeginInfo pass{.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=ctx.render_pass,.framebuffer=frame.framebuffer,.renderArea={{0,0},ctx.swapchain_extent},
        .clearValueCount=2,.pClearValues=clear};
    vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0,0,float(ctx.swapchain_extent.width),float(ctx.swapchain_extent.height),0,1};
    const VkRect2D scissor{{0,0},ctx.swapchain_extent};
    vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,lit_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,lit_layout,0,1,&scene_set,0,nullptr);
    for(size_t i=0;i<scene.objects.size();++i) {
        const auto& object=scene.objects[i]; if(object.floor&&!scene.show_floor) continue;
        const auto& geometry=object.floor?floor:octahedron;
        bind_geometry(command,geometry);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,lit_layout,1,1,&models[i].descriptor,0,nullptr);
        vkCmdDraw(command,geometry.count,1,0,0);
    }
    if(scene.show_markers) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,marker_pipeline); bind_geometry(command,cube);
        for(size_t i=0;i<scene.points.size();++i) marker(command,uniform.view_projection,scene.point_position(i),scene.points[i].color);
        for(const auto& s:scene.spots) marker(command,uniform.view_projection,s.position,s.color);
    }
    if(!lines.empty()) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,line_pipeline);
        const VkDeviceSize offset=0; vkCmdBindVertexBuffers(command,0,1,&line_buffer.handle,&offset);
        const MarkerPush push{uniform.view_projection,glm::vec4(1)};
        vkCmdPushConstants(command,unlit_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
        vkCmdDraw(command,uint32_t(lines.size()),1,0,0);
    }
    vkCmdEndRenderPass(command);
    if(vkEndCommandBuffer(command)!=VK_SUCCESS) throw std::runtime_error("Cannot finish frame");
}
size_t point_capacity(){return points_capacity;}
size_t spot_capacity(){return spots_capacity;}
size_t light_limit(bool spot){return size_t(max_storage_range/(spot?sizeof(SpotGPU):sizeof(PointGPU)));}
} // namespace lab2::renderer
