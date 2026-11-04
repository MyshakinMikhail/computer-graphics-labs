#include "renderer.hpp"
#include "image.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace lab3::renderer {
namespace {
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapping = nullptr;
    VkDeviceSize size = 0;
};
struct Geometry { Buffer vertices, indices; uint32_t count = 0; };
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
struct MarkerPush { glm::mat4 mvp; glm::vec4 color; };
static_assert(sizeof(MarkerPush) == 80);
Buffer scene_buffer, point_buffer, spot_buffer, model_buffer, line_buffer, line_staging;
Geometry octahedron, cube, floor;
std::vector<Texture> textures;
std::vector<VkDescriptorSet> material_sets;
VkSampler sampler = VK_NULL_HANDLE;
VkCommandPool upload_pool = VK_NULL_HANDLE;
size_t model_stride = 0, model_count = 0;
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
bool allocate_device_buffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
    const VkBufferCreateInfo info{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    const VmaAllocationCreateInfo alloc{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if(vmaCreateBuffer(graphics::internal::context.allocator,&info,&alloc,
        &buffer.handle,&buffer.allocation,nullptr)!=VK_SUCCESS) return false;
    buffer.size=size;
    return true;
}
void immediate(const std::function<void(VkCommandBuffer)>& record) {
    const auto& ctx=graphics::internal::context;
    VkCommandBuffer command=VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo allocation{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=upload_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(vkAllocateCommandBuffers(ctx.device,&allocation,&command)!=VK_SUCCESS)
        throw std::runtime_error("Cannot allocate upload commands");
    VkFence fence=VK_NULL_HANDLE;
    try {
        const VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS) throw std::runtime_error("Cannot begin upload");
        record(command);
        if(vkEndCommandBuffer(command)!=VK_SUCCESS) throw std::runtime_error("Cannot end upload");
        const VkFenceCreateInfo fence_info{.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if(vkCreateFence(ctx.device,&fence_info,nullptr,&fence)!=VK_SUCCESS)
            throw std::runtime_error("Cannot create upload fence");
        const VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&command};
        if(vkQueueSubmit(ctx.graphics_queue,1,&submit,fence)!=VK_SUCCESS)
            throw std::runtime_error("Cannot submit upload");
        if(vkWaitForFences(ctx.device,1,&fence,VK_TRUE,UINT64_MAX)!=VK_SUCCESS)
            throw std::runtime_error("Cannot wait for upload");
    } catch(...) {
        vkQueueWaitIdle(ctx.graphics_queue);
        if(fence) vkDestroyFence(ctx.device,fence,nullptr);
        vkFreeCommandBuffers(ctx.device,upload_pool,1,&command);
        throw;
    }
    vkDestroyFence(ctx.device,fence,nullptr);
    vkFreeCommandBuffers(ctx.device,upload_pool,1,&command);
}
void vertex_barrier(VkCommandBuffer command) {
    const VkMemoryBarrier barrier{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0,1,&barrier,0,nullptr,0,nullptr);
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
    geometry.count=uint32_t(mesh.indices.size());
    const VkDeviceSize vertex_bytes=mesh.vertices.size()*sizeof(Vertex);
    const VkDeviceSize index_bytes=mesh.indices.size()*sizeof(uint32_t);
    Buffer staging;
    if(!create_buffer(staging,vertex_bytes+index_bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    try {
        upload(staging,mesh.vertices.data(),size_t(vertex_bytes));
        std::memcpy(static_cast<char*>(staging.mapping)+vertex_bytes,mesh.indices.data(),size_t(index_bytes));
        if(vmaFlushAllocation(graphics::internal::context.allocator,staging.allocation,0,staging.size)!=VK_SUCCESS)
            throw std::runtime_error("Cannot flush geometry staging");
        if(!allocate_device_buffer(geometry.vertices,vertex_bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
           !allocate_device_buffer(geometry.indices,index_bytes,VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
            throw std::runtime_error("Cannot allocate device-local geometry");
        immediate([&](VkCommandBuffer command) {
            const VkBufferCopy vertices{0,0,vertex_bytes}, indices{vertex_bytes,0,index_bytes};
            vkCmdCopyBuffer(command,staging.handle,geometry.vertices.handle,1,&vertices);
            vkCmdCopyBuffer(command,staging.handle,geometry.indices.handle,1,&indices);
            vertex_barrier(command);
        });
    } catch(...) {destroy_buffer(staging);throw;}
    destroy_buffer(staging);
    return true;
}
size_t create_texture(const char* filename, VkFormat format) {
    const auto pixels=load_image(std::filesystem::path(LAB_TEXTURE_DIR)/filename);
    const auto& ctx=graphics::internal::context;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx.physical_device,&properties);
    if(uint32_t(pixels.width)>properties.limits.maxImageDimension2D ||
       uint32_t(pixels.height)>properties.limits.maxImageDimension2D)
        throw std::runtime_error("Texture exceeds device dimensions");
    // Store handles before creation so shutdown also releases partial initialization.
    textures.emplace_back();
    auto& texture=textures.back();
    const VkImageCreateInfo info{.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={uint32_t(pixels.width),uint32_t(pixels.height),1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    const VmaAllocationCreateInfo allocation{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    if(vmaCreateImage(ctx.allocator,&info,&allocation,&texture.image,&texture.allocation,nullptr)!=VK_SUCCESS)
        throw std::runtime_error("Cannot allocate texture image");
    Buffer staging;
    if(!create_buffer(staging,pixels.rgba.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Cannot allocate texture staging");
    try {
        upload(staging,pixels.rgba.data(),pixels.rgba.size());
        immediate([&](VkCommandBuffer command) {
            VkImageMemoryBarrier barrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=texture.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,0,nullptr,0,nullptr,1,&barrier);
            const VkBufferImageCopy region{.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .imageExtent=info.extent};
            vkCmdCopyBufferToImage(command,staging.handle,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,0,nullptr,0,nullptr,1,&barrier);
        });
    } catch(...) {destroy_buffer(staging);throw;}
    destroy_buffer(staging);
    const VkImageViewCreateInfo view{.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=texture.image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if(vkCreateImageView(ctx.device,&view,nullptr,&texture.view)!=VK_SUCCESS)
        throw std::runtime_error("Cannot create texture view");
    std::cout<<"TEXTURE: "<<filename<<" ("<<pixels.width<<'x'<<pixels.height<<")\n";
    return textures.size()-1;
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
    const auto device=graphics::internal::context.device;
    const VkDescriptorSetLayoutBinding scene_bindings[] = {
        {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo scene_info{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=3,.pBindings=scene_bindings};
    if(vkCreateDescriptorSetLayout(device,&scene_info,nullptr,&scene_layout)!=VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding material_bindings[] = {
        {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo model_info{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=4,.pBindings=material_bindings};
    if(vkCreateDescriptorSetLayout(device,&model_info,nullptr,&model_layout)!=VK_SUCCESS) return false;
    const auto count=uint32_t(material_files().size());
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,count},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3*count}};
    const VkDescriptorPoolCreateInfo pool_info{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=count+1,.poolSizeCount=4,.pPoolSizes=sizes};
    if(vkCreateDescriptorPool(device,&pool_info,nullptr,&descriptor_pool)!=VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayout> layouts(count+1,model_layout);
    layouts[0]=scene_layout;
    std::vector<VkDescriptorSet> sets(count+1);
    const VkDescriptorSetAllocateInfo allocate{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptor_pool,.descriptorSetCount=count+1,.pSetLayouts=layouts.data()};
    if(vkAllocateDescriptorSets(device,&allocate,sets.data())!=VK_SUCCESS) return false;
    scene_set=sets[0];
    material_sets.assign(sets.begin()+1,sets.end());
    if(!create_buffer(scene_buffer,sizeof(SceneUniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    bind_buffer(scene_set,0,scene_buffer,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(graphics::internal::context.physical_device,&properties);
    model_stride=uniform_stride(sizeof(ModelUniform),size_t(properties.limits.minUniformBufferOffsetAlignment));
    model_count=object_count;
    if(model_count==0 || model_stride>std::numeric_limits<uint32_t>::max()/model_count)
        throw std::runtime_error("Dynamic model offsets exceed uint32 range");
    if(sizeof(ModelUniform)>properties.limits.maxUniformBufferRange)
        throw std::runtime_error("Model block exceeds device uniform range");
    if(!create_buffer(model_buffer,model_stride*model_count,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    const VkSamplerCreateInfo sampler_info{.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT,.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT,.maxLod=0};
    if(vkCreateSampler(device,&sampler_info,nullptr,&sampler)!=VK_SUCCESS) return false;
    const size_t black=create_texture("black.png",VK_FORMAT_R8G8B8A8_UNORM);
    for(size_t i=0;i<material_files().size();++i) {
        const auto& files=material_files()[i];
        const size_t albedo=create_texture(files.albedo,VK_FORMAT_R8G8B8A8_SRGB);
        const size_t specular=files.specular?create_texture(files.specular,VK_FORMAT_R8G8B8A8_UNORM):black;
        const size_t emissive=files.emissive?create_texture(files.emissive,VK_FORMAT_R8G8B8A8_SRGB):black;
        // Range is ONE model block, never VK_WHOLE_SIZE with a nonzero dynamic offset.
        const VkDescriptorBufferInfo model{model_buffer.handle,0,sizeof(ModelUniform)};
        const VkWriteDescriptorSet uniform_write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet=material_sets[i],.dstBinding=0,.descriptorCount=1,
            .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&model};
        vkUpdateDescriptorSets(device,1,&uniform_write,0,nullptr);
        const size_t images[]{albedo,specular,emissive};
        for(uint32_t binding=1;binding<=3;++binding) {
            const VkDescriptorImageInfo image{sampler,textures[images[binding-1]].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkWriteDescriptorSet write{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet=material_sets[i],.dstBinding=binding,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
    }
    std::cout<<"DYNAMIC UBO: "<<model_count<<" models, block="<<sizeof(ModelUniform)
             <<", stride="<<model_stride<<", alignment="<<properties.limits.minUniformBufferOffsetAlignment<<'\n';
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
        {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},
        {2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)}};
    const VkPipelineVertexInputStateCreateInfo vertex{.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,
        .vertexAttributeDescriptionCount=lit?3u:1u,.pVertexAttributeDescriptions=attributes};
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
    vkCmdBindVertexBuffers(command,0,1,&geometry.vertices.handle,&offset);
    vkCmdBindIndexBuffer(command,geometry.indices.handle,0,VK_INDEX_TYPE_UINT32);
}
void marker(VkCommandBuffer command, glm::mat4 vp, glm::vec3 position, glm::vec3 color) {
    const auto model = glm::scale(glm::translate(glm::mat4(1),position),glm::vec3(0.09f));
    const MarkerPush push{vp*model,glm::vec4(color,1)};
    vkCmdPushConstants(command,unlit_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDrawIndexed(command,cube.count,1,0,0,0);
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
    const VkCommandPoolCreateInfo pool{.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=graphics::internal::context.graphics_queue_index};
    if(vkCreateCommandPool(graphics::internal::context.device,&pool,nullptr,&upload_pool)!=VK_SUCCESS) return false;
    bool ok = create_geometry(octahedron,octahedron_mesh()) && create_geometry(cube,cube_mesh()) &&
        create_geometry(floor,floor_mesh()) && create_descriptors(scene.objects.size());
    if(ok) ok=ensure_storage(point_buffer,points_capacity,scene.points.size(),sizeof(PointGPU),1) &&
              ensure_storage(spot_buffer,spots_capacity,scene.spots.size(),sizeof(SpotGPU),2);
    const VkDescriptorSetLayout layouts[]{scene_layout,model_layout};
    const VkPushConstantRange sampling_range{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(SamplingPush)};
    const VkPipelineLayoutCreateInfo lit_info{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=2,.pSetLayouts=layouts,.pushConstantRangeCount=1,.pPushConstantRanges=&sampling_range};
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
    if(sampler) vkDestroySampler(ctx.device,sampler,nullptr);
    for(const auto& texture:textures) {
        if(texture.view) vkDestroyImageView(ctx.device,texture.view,nullptr);
        if(texture.image) vmaDestroyImage(ctx.allocator,texture.image,texture.allocation);
    }
    textures.clear();material_sets.clear();
    if(upload_pool) vkDestroyCommandPool(ctx.device,upload_pool,nullptr);
    for(auto* b:{&scene_buffer,&point_buffer,&spot_buffer,&model_buffer,&line_buffer,&line_staging,
        &octahedron.vertices,&octahedron.indices,&cube.vertices,&cube.indices,&floor.vertices,&floor.indices}) destroy_buffer(*b);
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
    if(scene.objects.size()!=model_count) throw std::runtime_error("Scene model count changed after allocation");
    for(size_t i=0;i<scene.objects.size();++i) {
        const auto& object=scene.objects[i];
        const auto m=object.model(scene.angle);
        const ModelUniform data{m,glm::transpose(glm::inverse(m)),glm::vec4(object.material.albedo,1),
            glm::vec4(object.material.specular,std::max(1.0f,object.material.shininess))};
        std::memcpy(static_cast<char*>(model_buffer.mapping)+i*model_stride,&data,sizeof(data));
    }
    if(vmaFlushAllocation(ctx.allocator,model_buffer.allocation,0,model_buffer.size)!=VK_SUCCESS)
        throw std::runtime_error("Cannot flush dynamic model uniform");
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
            if(!allocate_device_buffer(replacement,std::max(VkDeviceSize(bytes),line_buffer.size*2),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
                throw std::runtime_error("Cannot grow cone geometry buffer");
            destroy_buffer(line_buffer); line_buffer=replacement;
            destroy_buffer(line_staging);
            if(!create_buffer(line_staging,line_buffer.size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
                throw std::runtime_error("Cannot allocate cone staging");
        }
        upload(line_staging,lines.data(),bytes);
    }
    const auto command=frame.command_buffer;
    if(vkResetCommandBuffer(command,0)!=VK_SUCCESS) throw std::runtime_error("Cannot reset frame commands");
    const VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS) throw std::runtime_error("Cannot begin frame");
    if(!lines.empty()) {
        const VkBufferCopy copy{0,0,lines.size()*sizeof(Vertex)};
        vkCmdCopyBuffer(command,line_staging.handle,line_buffer.handle,1,&copy);
        vertex_barrier(command);
    }
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
        const auto material=size_t(object.texture.material);
        if(material>=material_sets.size()) throw std::runtime_error("Invalid material index");
        const auto offset=uint32_t(i*model_stride);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,lit_layout,1,1,&material_sets[material],1,&offset);
        const auto push=sampling_push(object.texture,scene.texture_time);
        vkCmdPushConstants(command,lit_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
        vkCmdDrawIndexed(command,geometry.count,1,0,0,0);
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
} // namespace lab3::renderer
