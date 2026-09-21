#include "VkRendererBackend.h"

#include <chrono>
#include <thread>
#include <array>

#include <SDL_vulkan.h>

#include <RendererBackend/Vulkan/VkInitializers.h>
#include <RendererBackend/Vulkan/VkTypes.h>
#include <RendererBackend/Vulkan/VkImages.h>
#include "VkBootstrap.h"

#define VOLK_IMPLEMENTATION

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include "camera.h"

#include <glm/gtx/transform.hpp>

#include <Renderer/GlobalGPUTypes.h>

constexpr bool useValidationLayers = true;

static size_t hashPipelineKey(const SK::VkRendererBackend::PipelineKey& k)
{
    size_t h = 0;
    auto hc = [&](auto v)
    {
        std::hash<std::decay_t<decltype(v)>> hasher;
        h ^= hasher(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    };

    for (SK::VkRendererBackend::Shader shader : k.shaders)
    {
        hc(shader.module);
    }
    hc(k.topology);
    hc(k.polygonMode);
    hc(k.cullMode);
    hc(k.frontFace);
    hc(k.depthTest);
    hc(k.depthWrite);
    hc(k.depthCompare);
    hc(k.blending);
    hc(k.colorFormat);
    hc(k.depthFormat);

    for (SK::VkRendererBackend::ShaderResourceMapping mapping : k.shaderResourceMappings)
    {
        hc(mapping.binding);
        hc(mapping.type);
    }

    return h;
}

static void hashCombine(size_t* seed, size_t value)
{
    *seed ^= value + 0x9e3779b9 + (*seed << 6) + (*seed >> 2);
}

static size_t hashSamplerInfo(const VkSamplerCreateInfo& samplerInfo)
{
    size_t hash = 0;

    std::hash<uint64_t> integerHasher;
    std::hash<float> floatHasher;

    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.magFilter)));
    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.minFilter)));
    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.mipmapMode)));
    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.addressModeU)));
    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.addressModeV)));
    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.addressModeW)));
    hashCombine(&hash, floatHasher(samplerInfo.mipLodBias));
    hashCombine(&hash, integerHasher(samplerInfo.anisotropyEnable ? 1 : 0));
    hashCombine(&hash, floatHasher(samplerInfo.maxAnisotropy));
    hashCombine(&hash, integerHasher(samplerInfo.compareEnable ? 1 : 0));
    hashCombine(&hash, floatHasher(samplerInfo.minLod));
    hashCombine(&hash, floatHasher(samplerInfo.maxLod));
    hashCombine(&hash, integerHasher(static_cast<uint64_t>(samplerInfo.borderColor)));

    return hash;
}


void SK::VkRendererBackend::init(State* vkRendererBackend, struct SDL_Window* window, uint32_t windowWidth, uint32_t windowHeight)
{
    // only one vkRendererBackend initialization is allowed with the application.
    assert(vkRendererBackend->isInitialized == false);

    // Store window data coming from the App for later use
    vkRendererBackend->window = window;
    vkRendererBackend->windowExtent = VkExtent2D{ windowWidth, windowHeight };

    // Vulkan Bootstrapping
    initVulkan(vkRendererBackend);
    initSwapchain(vkRendererBackend);
    initCommands(vkRendererBackend);
    // initSwapchain sets the number of swapchain images which is needed to create correct amount of submitSemaphores. So, it needs to be called before initSyncStructures.
    initSyncStructures(vkRendererBackend);

    createDrawAndDepthImages(vkRendererBackend);

    initDescriptors(vkRendererBackend);

    initDefaultData(vkRendererBackend);

    initGlobalSceneBuffer(vkRendererBackend);
    //initGlobalSceneBuffer2(vkRendererBackend);

    // everything went fine
    vkRendererBackend->isInitialized = true;
}

void SK::VkRendererBackend::shutdown(State* vkRendererBackend)
{
    if(vkRendererBackend->isInitialized) 
    {
        for(int i = 0; i < FRAME_OVERLAP; ++i)
        {
            // Destroy sync objects
            vkDestroyFence(vkRendererBackend->device, vkRendererBackend->frames[i].renderFence, nullptr);
            vkDestroySemaphore(vkRendererBackend->device, vkRendererBackend->frames[i].swapchainAcquireSemaphore, nullptr);

            // It’s not possible to individually destroy VkCommandBuffer, destroying their parent pool will destroy all of the command buffers allocated from it.
            vkDestroyCommandPool(vkRendererBackend->device, vkRendererBackend->frames[i].commandPool, nullptr);

            vkRendererBackend->frames[i].deletionQueue.flush();
        }

        for (int i = 0; i < vkRendererBackend->numSwapchainImages; ++i)
        {
            vkDestroySemaphore(vkRendererBackend->device, vkRendererBackend->submitSemaphores[i], nullptr);
        }
        vkRendererBackend->submitSemaphores.clear();

        // Clear out the caches
        clearShaderCache(vkRendererBackend);
        clearPipelineCache(vkRendererBackend);

        destroyDrawAndDepthImages(vkRendererBackend);

        // destroying the vma allocator is also inside the mainDeletionQueue, so any resource allocation must be freed before flushing the queue
        vkRendererBackend->mainDeletionQueue.flush();

        destroySwapchain(vkRendererBackend);

        vkDestroyDevice(vkRendererBackend->device, nullptr);
        vkDestroySurfaceKHR(vkRendererBackend->instance, vkRendererBackend->surface, nullptr);

        vkb::destroy_debug_utils_messenger(vkRendererBackend->instance, vkRendererBackend->debugMessenger);
        vkDestroyInstance(vkRendererBackend->instance, nullptr);

        volkFinalize();

        // Nullify non-owning pointers
        vkRendererBackend->window = nullptr;
        
        vkRendererBackend->isInitialized = false;
    }
}

/*
    Prepares/synchronizes internal logic and prepares the frame to be drawn.

    Returns true if the frame begun successfully. In the cases like swapchain resize, it returns false.
*/
bool SK::VkRendererBackend::beginFrame(State* vkRendererBackend)
{
    FrameData& currentFrame = getCurrentFrameData(vkRendererBackend);
    // Wait until the GPU has finished rendering the last frame of the same modularity (0->1->2->3  wait on 2 for 0 and wait on 3 for 1 and so on)
    VK_CHECK(vkWaitForFences(vkRendererBackend->device, 1, &currentFrame.renderFence, true, 1000000000));

    currentFrame.deletionQueue.flush();

    // To be able to use the same fence it must be reset after use
    VK_CHECK(vkResetFences(vkRendererBackend->device, 1, &currentFrame.renderFence));

    // Request an available image from the swapchain. swapchainAcquireSemaphore is signaled once it has finished presenting the image so it can be used again.
    // More detailed description of how vkAcquireNextImageKHR works: https://stackoverflow.com/questions/60419749/why-does-vkacquirenextimagekhr-never-block-my-thread
    uint32_t swapchainImageIndex;
    VkResult acquireResult = vkAcquireNextImageKHR(vkRendererBackend->device, vkRendererBackend->swapchain, 1000000000, currentFrame.swapchainAcquireSemaphore, nullptr, &swapchainImageIndex);
    if(acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        vkRendererBackend->windowResizeRequested = true;
        return false;
    }

    // Vulkan handles are just a 64 bit handles/pointers, so its fine to copy them around, but remember that their actual data is handled by vulkan itself.
    VkCommandBuffer cmd = currentFrame.mainCommandBuffer;

    // Now we are sure that command is executed, we can safely reset it and begin recording again
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    // Begin the command buffer recording. We will submit this command buffer exactly once, so we let Vulkan know that
    VkCommandBufferBeginInfo cmdBeginInfo = SK::VkInit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    // Start the command buffer recording
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    // Transition depth image to optimal depth layout
    SK::VkUtil::transitionImage(cmd, vkRendererBackend->depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    // Transition draw image to optimal rendering layout
    SK::VkUtil::transitionImage(cmd, vkRendererBackend->drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Frame has begun successfully, fill in per-frame transient state in the renderer backend state
    vkRendererBackend->currentCmdBuffer = currentFrame.mainCommandBuffer;
    vkRendererBackend->currentSwapchainImageIndex = swapchainImageIndex;

    // Bind the descriptor heaps once per-frame.
    SK::VkRendererBackend::bindDescriptorHeap(vkRendererBackend, cmd, &vkRendererBackend->descriptorHeap);

    return true;
}

void SK::VkRendererBackend::endFrame(State* vkRendererBackend)
{
    FrameData& currentFrame = getCurrentFrameData(vkRendererBackend);

    VkCommandBuffer cmd = vkRendererBackend->currentCmdBuffer;
    uint32_t swapchainImageIndex = vkRendererBackend->currentSwapchainImageIndex;

    // Transition swapchain image into the presentation layout
    SK::VkUtil::transitionImage(cmd, vkRendererBackend->swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Finalize the command buffer
    VK_CHECK(vkEndCommandBuffer(cmd));

    // Prepare the submission
    // GPU will wait on the swapchainAcquireSemaphore before outputting the final colors. swapchainAcquireSemaphore is set to be signalled in vkAcquireSwapchainImage once the swapchain is done presenting that image.
    // GPU will signal submitSemaphore to signal that rendering has finished
    VkCommandBufferSubmitInfo cmdSubmitInfo = SK::VkInit::command_buffer_submit_info(cmd);
    VkSemaphoreSubmitInfo waitInfo = SK::VkInit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, currentFrame.swapchainAcquireSemaphore);
    VkSemaphoreSubmitInfo signalInfo = SK::VkInit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, vkRendererBackend->submitSemaphores[swapchainImageIndex]);
    VkSubmitInfo2 submit = SK::VkInit::submit_info(&cmdSubmitInfo, &signalInfo, &waitInfo);

    // Submit command buffer to the queue and execute it
    // renderFence will be signaled once the submitted command buffer has completed execution.
    VK_CHECK(vkQueueSubmit2(vkRendererBackend->graphicsQueue, 1, &submit, currentFrame.renderFence));

    // Prepare the presentation
    // GPU will wait on the submitSemaphore so that it will be guaranteed that the rendering has been finished and the swapchain image is ready to be presented
    VkPresentInfoKHR presentInfo = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = nullptr };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vkRendererBackend->swapchain;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &vkRendererBackend->submitSemaphores[swapchainImageIndex];
    presentInfo.pImageIndices = &swapchainImageIndex;

    VkResult presentResult = vkQueuePresentKHR(vkRendererBackend->graphicsQueue, &presentInfo);
    if(presentResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        vkRendererBackend->windowResizeRequested = true;
    }

    // Increase the number of frames drawn
    ++vkRendererBackend->frameNumber;
}

void SK::VkRendererBackend::immediateSubmit(State* vkRendererBackend, std::function<void(VkCommandBuffer cmd)>&& function)
{
    // Before starting submitting and waiting on the fence reset them
    VK_CHECK(vkResetFences(vkRendererBackend->device, 1, &vkRendererBackend->immeadiateFence));
    VK_CHECK(vkResetCommandBuffer(vkRendererBackend->immediateCommandBuffer, 0));
    // Prepare the immediate command buffer for executing function given as the param
    VkCommandBuffer cmd = vkRendererBackend->immediateCommandBuffer;
    VkCommandBufferBeginInfo cmdBeginInfo = SK::VkInit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
    function(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    // Submit
    VkCommandBufferSubmitInfo cmdSubmitInfo = SK::VkInit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submitInfo = SK::VkInit::submit_info(&cmdSubmitInfo, nullptr, nullptr);
    VK_CHECK(vkQueueSubmit2(vkRendererBackend->graphicsQueue, 1, &submitInfo, vkRendererBackend->immeadiateFence));

    // Wait on the fence until the command buffer finished executing
    VK_CHECK(vkWaitForFences(vkRendererBackend->device, 1, &vkRendererBackend->immeadiateFence, true, 9999999999));
}

AllocatedBuffer SK::VkRendererBackend::createBuffer(State* vkRendererBackend, size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    // Allocate buffer
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr };
    bufferInfo.size = allocSize;
    bufferInfo.usage = usage;
    // The address of every buffer is cached in this engine.
    bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo vmaAllocInfo = {};
    vmaAllocInfo.usage = memoryUsage;
    vmaAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    AllocatedBuffer newBuffer;

    VK_CHECK(vmaCreateBuffer(vkRendererBackend->vmaAllocator, &bufferInfo, &vmaAllocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.allocInfo));

    // cache the address of the buffer
    VkBufferDeviceAddressInfo deviceAddressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = newBuffer.buffer };
    newBuffer.address = vkGetBufferDeviceAddress(vkRendererBackend->device, &deviceAddressInfo);

    return newBuffer;
}

AllocatedBuffer SK::VkRendererBackend::createAndUploadGPUBuffer(State* vkRendererBackend, size_t allocSize, VkBufferUsageFlags usage, const void* data, size_t srcOffset, size_t dstOffset)
{
    AllocatedBuffer newBuffer = createBuffer(vkRendererBackend, allocSize, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    AllocatedBuffer staging = createBuffer(vkRendererBackend, allocSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* pStaging = staging.allocation->GetMappedData();

    // Copy The Lookup Table into the staging buffer
    memcpy(pStaging, data, allocSize);

    immediateSubmit(vkRendererBackend, [&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.dstOffset = dstOffset;
        copy.srcOffset = srcOffset;
        copy.size = allocSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newBuffer.buffer, 1, &copy);
    });

    destroyBuffer(vkRendererBackend, staging);

    return newBuffer;
}

AllocatedBuffer SK::VkRendererBackend::uploadStagingBuffer(State* vkRendererBackend, VkBuffer stagingBuffer, size_t allocSize, VkBufferUsageFlags usage, size_t srcOffset, size_t dstOffset)
{
    AllocatedBuffer newBuffer = createBuffer(vkRendererBackend, allocSize, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    immediateSubmit(vkRendererBackend, [&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.dstOffset = dstOffset;
        copy.srcOffset = srcOffset;
        copy.size = allocSize;

        vkCmdCopyBuffer(cmd, stagingBuffer, newBuffer.buffer, 1, &copy);
    });

    return newBuffer;
}

void SK::VkRendererBackend::destroyBuffer(State* vkRendererBackend, const AllocatedBuffer& buffer)
{
    vmaDestroyBuffer(vkRendererBackend->vmaAllocator, buffer.buffer, buffer.allocation);
}

AllocatedImage SK::VkRendererBackend::createImage(State* vkRendererBackend, VkExtent3D imageExtent, VkFormat format, VkImageUsageFlags usage, bool mipMapped)
{
    AllocatedImage newImage;
    newImage.imageFormat = format;
    newImage.imageExtent = imageExtent;

    VkImageCreateInfo imgInfo = SK::VkInit::image_create_info(format, usage, imageExtent);
    if(mipMapped)
    {
        imgInfo.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(imageExtent.width, imageExtent.height)))) + 1;
        newImage.mipLevels = imgInfo.mipLevels;
    }

    // Always allocate images on dedicated GPU memory
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Allocate and create the image
    VK_CHECK(vmaCreateImage(vkRendererBackend->vmaAllocator, &imgInfo, &allocInfo, &newImage.image, &newImage.allocation, nullptr));

    // Defaulting to the color aspect unless depth format is given
    VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT; 
    if(format == VK_FORMAT_D32_SFLOAT) // if the format is the depth format
    {
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    // Create the image-view for the image
    VkImageViewCreateInfo viewInfo = SK::VkInit::imageview_create_info(format, newImage.image, aspectFlag);
    viewInfo.subresourceRange.levelCount = imgInfo.mipLevels;

    VK_CHECK(vkCreateImageView(vkRendererBackend->device, &viewInfo, nullptr, &newImage.imageView));

    return newImage;
}

AllocatedImage SK::VkRendererBackend::createImage(State* vkRendererBackend, const void* data, size_t dataSize, VkExtent3D imageExtent, VkFormat format, VkImageUsageFlags usage, bool mipMapped)
{
    AllocatedBuffer uploadBuffer = createBuffer(vkRendererBackend, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    memcpy(uploadBuffer.allocInfo.pMappedData, data, dataSize);

    // aside from the original usage also allow copying data into and from it.
    AllocatedImage newImage = createImage(vkRendererBackend, imageExtent, format, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mipMapped);

    // Perform a buffer to image copy.
    immediateSubmit(vkRendererBackend, [&](VkCommandBuffer cmd) {
        SK::VkUtil::transitionImage(cmd, newImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion{};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;

        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = imageExtent;

        // copy buffer to image
        vkCmdCopyBufferToImage(cmd, uploadBuffer.buffer, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        if(mipMapped)
        {
            SK::VkUtil::generateMipmaps(cmd, newImage.image, VkExtent2D{ newImage.imageExtent.width, newImage.imageExtent.height });
        }
        else
        {
            SK::VkUtil::transitionImage(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

    });

    destroyBuffer(vkRendererBackend, uploadBuffer);

    return newImage;
}

void SK::VkRendererBackend::destroyImage(State* vkRendererBackend, const AllocatedImage& img)
{
    vkDestroyImageView(vkRendererBackend->device, img.imageView, nullptr);
    vmaDestroyImage(vkRendererBackend->vmaAllocator, img.image, img.allocation);
}

VkImageViewCreateInfo SK::VkRendererBackend::createImageViewInfo(State* vkRendererBackend, const AllocatedImage& image)
{
    // Defaulting to the color aspect unless depth format is given
    VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
    if (image.imageFormat == VK_FORMAT_D32_SFLOAT) // if the format is the depth format
    {
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    // Create the image-view for the image
    VkImageViewCreateInfo viewInfo = SK::VkInit::imageview_create_info(image.imageFormat, image.image, aspectFlag);
    viewInfo.subresourceRange.levelCount = image.mipLevels;
    
    return viewInfo;
}

SK::VkRendererBackend::SamplerDescriptorHandle SK::VkRendererBackend::createSamplerDescriptor(State* vkRendererBackend, const VkSamplerCreateInfo& samplerInfo)
{
    size_t samplerHash = hashSamplerInfo(samplerInfo);
    auto existing = vkRendererBackend->samplerDescriptorCache.find(samplerHash);
    if (existing != vkRendererBackend->samplerDescriptorCache.end())
    {
        return existing->second;
    }

    SamplerDescriptorHandle handle = allocateSamplerDescriptor(&vkRendererBackend->descriptorHeap);
    writeSamplerDescriptor(vkRendererBackend, &vkRendererBackend->descriptorHeap, handle, samplerInfo);
    vkRendererBackend->samplerDescriptorCache[samplerHash] = handle;

    return handle;
}

SK::VkRendererBackend::SamplerDescriptorHandle SK::VkRendererBackend::createSamplerDescriptor(State* vkRendererBackend, VkFilter minFilter, VkFilter magFilter, VkSamplerMipmapMode mipmapMode, VkSamplerAddressMode addressMode)
{
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

    samplerInfo.minFilter = minFilter;
    samplerInfo.magFilter = magFilter;
    samplerInfo.mipmapMode = mipmapMode;
    samplerInfo.addressModeU = addressMode;
    samplerInfo.addressModeV = addressMode;
    samplerInfo.addressModeW = addressMode;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 16.0f;
    samplerInfo.anisotropyEnable = mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxAnisotropy = mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? 4.0f : 1.0f;

    return createSamplerDescriptor(vkRendererBackend, samplerInfo);
}

VkGPUMeshBuffers SK::VkRendererBackend::uploadMesh(State* vkRendererBackend, std::span<SK::Renderer::Vertex> vertices, std::span<uint32_t> indices)
{
    const size_t vertexBufferSize = vertices.size() * sizeof(SK::Renderer::Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    VkGPUMeshBuffers meshBuffers;

    // Create the vertex buffer and fetch the device address of it
    meshBuffers.vertexBuffer = createBuffer(vkRendererBackend, vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    // Create the index buffer
    meshBuffers.indexBuffer = createBuffer(vkRendererBackend, indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    AllocatedBuffer staging = createBuffer(vkRendererBackend, vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* data = staging.allocation->GetMappedData();

    // Copy Vertex Buffer
    memcpy(data, vertices.data(), vertexBufferSize);
    // Copy Index Buffer
    memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

    immediateSubmit(vkRendererBackend, [&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{};
        vertexCopy.dstOffset = 0;
        vertexCopy.srcOffset = 0;
        vertexCopy.size = vertexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, meshBuffers.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{};
        indexCopy.dstOffset = 0;
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.size = indexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, meshBuffers.indexBuffer.buffer, 1, &indexCopy);
    });

    destroyBuffer(vkRendererBackend, staging);

    return meshBuffers;
}

/*
    Both update and bind scene buffer functions must be called after the frame fence waits as it will be guaranteed that the frame is done being used by GPU. Otherwise, the data can be corrupted. 
    So, it can be safely called after frameBegin function
*/
void SK::VkRendererBackend::updateSceneBuffer(State* vkRendererBackend, const SK::Renderer::GPUSceneData & sceneData)
{
    // Update the scene buffer
    SK::Renderer::GPUSceneData* pGpuSceneDataBuffer = (SK::Renderer::GPUSceneData*)vkRendererBackend->gpuSceneDataBuffer.allocation->GetMappedData();
    pGpuSceneDataBuffer[vkRendererBackend->frameNumber % FRAME_OVERLAP] = sceneData;
}

void SK::VkRendererBackend::setViewport(State* vkRendererBackend, VkCommandBuffer cmd)
{
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = vkRendererBackend->drawExtent.width;
    viewport.height = vkRendererBackend->drawExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
}

void SK::VkRendererBackend::setScissor(State* vkRendererBackend, VkCommandBuffer cmd)
{
    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = vkRendererBackend->drawExtent.width;
    scissor.extent.height = vkRendererBackend->drawExtent.height;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void SK::VkRendererBackend::createDrawAndDepthImages(State* vkRendererBackend)
{
    // draw image size will match the window
    VkExtent3D drawImageExtent = {
        vkRendererBackend->windowExtent.width,
        vkRendererBackend->windowExtent.height,
        1
    };

    vkRendererBackend->drawExtent = VkExtent2D{ vkRendererBackend->windowExtent.width, vkRendererBackend->windowExtent.height };
    // Initialize the draw image
    vkRendererBackend->drawImage = createImage(vkRendererBackend, drawImageExtent, VK_FORMAT_R16G16B16A16_SFLOAT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    // Initialize the depth image
    vkRendererBackend->depthImage = createImage(vkRendererBackend, drawImageExtent, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

void SK::VkRendererBackend::destroyDrawAndDepthImages(State* vkRendererBackend)
{
    // Destroy the Draw Image
    vmaDestroyImage(vkRendererBackend->vmaAllocator, vkRendererBackend->drawImage.image, vkRendererBackend->drawImage.allocation);
    vkDestroyImageView(vkRendererBackend->device, vkRendererBackend->drawImage.imageView, nullptr);
    // Destroy the Depth Image
    vmaDestroyImage(vkRendererBackend->vmaAllocator, vkRendererBackend->depthImage.image, vkRendererBackend->depthImage.allocation);
    vkDestroyImageView(vkRendererBackend->device, vkRendererBackend->depthImage.imageView, nullptr);
}

SK::VkRendererBackend::FrameData& SK::VkRendererBackend::getCurrentFrameData(State* vkRendererBackend)
{
    return vkRendererBackend->frames[vkRendererBackend->frameNumber % FRAME_OVERLAP];
}

SK::VkRendererBackend::Shader SK::VkRendererBackend::getOrLoadShader(State* vkRendererBackend, const char* path, VkShaderStageFlagBits stage)
{
    size_t hash = std::hash<std::string>{}(path);

    auto it = vkRendererBackend->shaderCache.find(hash);
    if(it != vkRendererBackend->shaderCache.end())
    {
        return it->second;
    }

    Shader shader;
    shader.stage = stage;
    if(!SK::VkUtil::loadShaderModule(vkRendererBackend->device, path, &shader.module))
    {
        return Shader{ .module = VK_NULL_HANDLE, .stage = stage };
    }

    vkRendererBackend->shaderCache[hash] = shader;
    return shader;
}
    
void SK::VkRendererBackend::clearShaderCache(State* vkRendererBackend)
{
    for(auto& [k, s] : vkRendererBackend->shaderCache)
    {
        vkDestroyShaderModule(vkRendererBackend->device, s.module, nullptr);
    }

    vkRendererBackend->shaderCache.clear();
}

VkPipeline SK::VkRendererBackend::getOrCreatePipeline(State* vkRendererBackend, const PipelineKey& key)
{
    size_t hash = hashPipelineKey(key);

    auto it = vkRendererBackend->pipelineCache.find(hash);
    if(it != vkRendererBackend->pipelineCache.end())
    {
        return it->second;
    }

    PipelineBuilder builder;
    builder.clear();

    for (const Shader& shader : key.shaders)
    {
        builder.pushShaderStage(shader.module, shader.stage);
    }

    builder.setInputTopology(key.topology);
    builder.setPolygonMode(key.polygonMode);
    builder.setCullMode(key.cullMode, key.frontFace);
    // Hardcoding for now
    builder.setMultiSamplingNone();

    if(key.blending)
    {
        builder.enableBlendingAdditive();
    }
    else
    {
        builder.disableBlending();
    }

    if(key.depthTest)
    {
        builder.enableDepthTest(key.depthWrite, key.depthCompare);
    }
    else
    {
        builder.disableDepthTest();
    }

    builder.setColorAttachmentFormat(key.colorFormat);
    builder.setDepthFormat(key.depthFormat);

    for (const SK::VkRendererBackend::ShaderResourceMapping mapping : key.shaderResourceMappings)
    {
        uint32_t heapArrayStride = mapping.type != VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT ? vkRendererBackend->descriptorHeap.resourceDescriptorStride : vkRendererBackend->descriptorHeap.samplerDescriptorSize;
        builder.pushShaderResourceMapping(mapping, heapArrayStride);
    }

    VkPipeline pipeline = builder.buildPipeline(vkRendererBackend->device);

    vkRendererBackend->pipelineCache[hash] = pipeline;
    return pipeline;
}

void SK::VkRendererBackend::clearPipelineCache(State* vkRendererBackend)
{
    for(auto& [k, p] : vkRendererBackend->pipelineCache)
    {
        vkDestroyPipeline(vkRendererBackend->device, p, nullptr);
    }

    vkRendererBackend->pipelineCache.clear();
}

void SK::VkRendererBackend::initVulkan(State* vkRendererBackend)
{
    VK_CHECK(volkInitialize());

    vkb::InstanceBuilder builder;

    // Create the Vulkan instance with basic debug features.
    auto instRet = builder.set_app_name("Vulkan Engine")
        .request_validation_layers(useValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 4, 0)
        .build();

    vkb::Instance vkbInstance = instRet.value();
    volkLoadInstanceOnly(vkbInstance.instance);

    // Grab the instance
    vkRendererBackend->instance = vkbInstance.instance;
    vkRendererBackend->debugMessenger = vkbInstance.debug_messenger;

    SDL_Vulkan_CreateSurface(vkRendererBackend->window, vkRendererBackend->instance, &vkRendererBackend->surface);

    // Vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = true;
    features13.synchronization2 = true;

    // Vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;
    features12.scalarBlockLayout = true;
    features12.runtimeDescriptorArray = true;
    features12.shaderSampledImageArrayNonUniformIndexing = true;
    features12.storageBuffer8BitAccess = true;
    features12.shaderInt8 = true;

    // Vulkan 1.0 features
    VkPhysicalDeviceFeatures features10{};
    features10.samplerAnisotropy = true;

    // Get extensions
    // Descriptor Heaps
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
        .pNext = nullptr,
        .descriptorHeap = true
    };

    // Use vkbootstrap to select a gpu with Vulkan 1.3 and necessary features
    vkb::PhysicalDeviceSelector selector{vkbInstance};
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 4)
        .set_required_features_13(features13)
        .set_required_features_12(features12)
        .set_required_features(features10)
        .add_required_extension(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)
        .add_required_extension_features(descriptorHeapFeatures)
        .set_surface(vkRendererBackend->surface)
        .select()
        .value();

    // Create the final Vulkan device
    vkb::DeviceBuilder deviceBuilder{physicalDevice};
    vkb::Device vkbDevice = deviceBuilder.build().value();
    volkLoadDevice(vkbDevice.device);

    // Get the VKDevice handle used in the rest of the Vulkan application
    vkRendererBackend->device = vkbDevice.device;
    vkRendererBackend->chosenGPU = vkbDevice.physical_device;
    // Get the Graphics Queue
    vkRendererBackend->graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    vkRendererBackend->graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // Initialize the vulkan memory allocator
    // When using volk, these two function pointers must be provided.
    const VmaVulkanFunctions vmaFunctions{
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
    };

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = vkRendererBackend->chosenGPU;
    allocatorInfo.device = vkRendererBackend->device;
    allocatorInfo.instance = vkRendererBackend->instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    vmaCreateAllocator(&allocatorInfo, &vkRendererBackend->vmaAllocator);

    vkRendererBackend->mainDeletionQueue.pushFunction([=](){
        vmaDestroyAllocator(vkRendererBackend->vmaAllocator);
    });
}

void SK::VkRendererBackend::initSwapchain(State* vkRendererBackend)
{
    createSwapchain(vkRendererBackend, vkRendererBackend->windowExtent.width, vkRendererBackend->windowExtent.height);
}

void SK::VkRendererBackend::initCommands(State* vkRendererBackend)
{
    // Create the command pool and allow for resetting of individual command buffers
    VkCommandPoolCreateInfo commandPoolInfo = SK::VkInit::command_pool_create_info(vkRendererBackend->graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for(int i = 0; i < FRAME_OVERLAP; ++i)
    {
        VK_CHECK(vkCreateCommandPool(vkRendererBackend->device, &commandPoolInfo, nullptr, &vkRendererBackend->frames[i].commandPool));
        // Allocate the default command buffer that will be used for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = SK::VkInit::command_buffer_allocate_info(vkRendererBackend->frames[i].commandPool, 1);
        VK_CHECK(vkAllocateCommandBuffers(vkRendererBackend->device, &cmdAllocInfo, &vkRendererBackend->frames[i].mainCommandBuffer));
    }

    // Immediate commands
    VK_CHECK(vkCreateCommandPool(vkRendererBackend->device, &commandPoolInfo, nullptr, &vkRendererBackend->immediateCommandPool));

    // Allocate a command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo = SK::VkInit::command_buffer_allocate_info(vkRendererBackend->immediateCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(vkRendererBackend->device, &cmdAllocInfo, &vkRendererBackend->immediateCommandBuffer));

    vkRendererBackend->mainDeletionQueue.pushFunction([=](){
        vkDestroyCommandPool(vkRendererBackend->device, vkRendererBackend->immediateCommandPool, nullptr);
    });
}

void SK::VkRendererBackend::initSyncStructures(State* vkRendererBackend)
{
    // Create synchronization structures
    // One fence to sync CPU-GPU when the gpu has finished rendering the frame,
    // 1 per-frame semaphore to sync swapchain ready image acquiring.
    // 1 per-swapchain image semaphore to sync swapchain image presentation. Presentation resources are per-swapchain image not per-frame. So, the synchronization should be done per-swapchain basis not frame basis.
    // We want the fence to start signalled so we can wait on it on the first frame
    VkFenceCreateInfo fenceCreateInfo = SK::VkInit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = SK::VkInit::semaphore_create_info();

    for(int i = 0; i < FRAME_OVERLAP; ++i)
    {
        VK_CHECK(vkCreateFence(vkRendererBackend->device, &fenceCreateInfo, nullptr, &vkRendererBackend->frames[i].renderFence));

        VK_CHECK(vkCreateSemaphore(vkRendererBackend->device, &semaphoreCreateInfo, nullptr, &vkRendererBackend->frames[i].swapchainAcquireSemaphore));
    }

    vkRendererBackend->submitSemaphores.resize(vkRendererBackend->numSwapchainImages);
    for (int i = 0; i < vkRendererBackend->numSwapchainImages; ++i)
    {
        VK_CHECK(vkCreateSemaphore(vkRendererBackend->device, &semaphoreCreateInfo, nullptr, &vkRendererBackend->submitSemaphores[i]));
    }

    // Fence for the immediate command buffers
    VK_CHECK(vkCreateFence(vkRendererBackend->device, &fenceCreateInfo, nullptr, &vkRendererBackend->immeadiateFence));
    vkRendererBackend->mainDeletionQueue.pushFunction([=](){
        vkDestroyFence(vkRendererBackend->device, vkRendererBackend->immeadiateFence, nullptr);
    });
}

void SK::VkRendererBackend::createSwapchain(State* vkRendererBackend, uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ vkRendererBackend->chosenGPU, vkRendererBackend->device, vkRendererBackend->surface};

    vkRendererBackend->swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        .set_desired_format(VkSurfaceFormatKHR{.format = vkRendererBackend->swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    
    vkRendererBackend->swapchainExtent = vkbSwapchain.extent;
    // Store the swapchain and its related images
    vkRendererBackend->swapchain = vkbSwapchain.swapchain;
    vkRendererBackend->swapchainImages = vkbSwapchain.get_images().value();
    vkRendererBackend->swapchainImageViews = vkbSwapchain.get_image_views().value();
    vkRendererBackend->numSwapchainImages = static_cast<uint32_t>(vkRendererBackend->swapchainImages.size());
}

void SK::VkRendererBackend::destroySwapchain(State* vkRendererBackend)
{
    // Deleting the swapchain deletes the images it holds internally.
    vkDestroySwapchainKHR(vkRendererBackend->device, vkRendererBackend->swapchain, nullptr);

    // Destroy the swapchain resources
    for(int i = 0; i < vkRendererBackend->swapchainImageViews.size(); ++i)
    {
        vkDestroyImageView(vkRendererBackend->device, vkRendererBackend->swapchainImageViews[i], nullptr);
    }

    vkRendererBackend->swapchainImages.clear();
    vkRendererBackend->swapchainImageViews.clear();
}

void SK::VkRendererBackend::handleWindowResize(State* vkRendererBackend)
{
    // Don't change the images and views while the gpu is still handling them
    vkDeviceWaitIdle(vkRendererBackend->device);

    int w, h;
    SDL_GetWindowSize(vkRendererBackend->window, &w, &h);
    vkRendererBackend->windowExtent.width = w;
    vkRendererBackend->windowExtent.height = h;

    // Recreate swapchain and draw, depth images.
    destroySwapchain(vkRendererBackend);
    createSwapchain(vkRendererBackend, vkRendererBackend->windowExtent.width, vkRendererBackend->windowExtent.height);
    destroyDrawAndDepthImages(vkRendererBackend);
    createDrawAndDepthImages(vkRendererBackend);

    vkRendererBackend->windowResizeRequested = false;
}

void SK::VkRendererBackend::initDescriptors(State* vkRendererBackend)
{
    DescriptorHeapDesc desc{ .maxResourceDescriptors = 8192, .maxSamplerDescriptors = 256 };
    initDescriptorHeap(vkRendererBackend, &vkRendererBackend->descriptorHeap, desc);
    vkRendererBackend->mainDeletionQueue.pushFunction([=]() {
        destroyDescriptorHeap(vkRendererBackend, &vkRendererBackend->descriptorHeap);
    });
}

void SK::VkRendererBackend::initDefaultData(State* vkRendererBackend)
{
    // Default textures
    // 3 default textures 1 pixel each
    size_t dataSize = 4; // 1 pixel RGBA 8 bit
    uint32_t white = glm::packUnorm4x8(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
    vkRendererBackend->whiteImage = createImage(vkRendererBackend, (void*)&white, dataSize, VkExtent3D{1, 1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1.0f));
    vkRendererBackend->greyImage = createImage(vkRendererBackend, (void*)&grey, dataSize, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t black = glm::packUnorm4x8(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
    vkRendererBackend->blackImage =createImage(vkRendererBackend, (void*)&black, dataSize, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

    //checkerboard image
    uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
    std::array<uint32_t, 16 * 16 > pixels; //for 16x16 checkerboard texture
    for(int y = 0; y < 16; ++y) 
    {
        for(int x = 0; x < 16; ++x) 
        {
            pixels[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
        }
    }

    dataSize = 16 * 16 * 1 * 4; // 16 x 16 RGBA 8 bit
    vkRendererBackend->errorCheckerboardImage = createImage(vkRendererBackend, pixels.data(), dataSize, VkExtent3D{ 16, 16, 1 }, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);

    vkRendererBackend->mainDeletionQueue.pushFunction([=]() {
        destroyImage(vkRendererBackend, vkRendererBackend->whiteImage);
        destroyImage(vkRendererBackend, vkRendererBackend->greyImage);
        destroyImage(vkRendererBackend, vkRendererBackend->blackImage);
        destroyImage(vkRendererBackend, vkRendererBackend->errorCheckerboardImage);
    });
}

void SK::VkRendererBackend::initGlobalSceneBuffer(State* vkRendererBackend)
{
    size_t bufferSize = FRAME_OVERLAP * sizeof(SK::Renderer::GPUSceneData);
    // Allocate a new uniform buffer for scene data (allocating on VRAM that CPU can write to directly. It is limited but it is perfect for allocating reasonable amounts that are dynamic)
    vkRendererBackend->gpuSceneDataBuffer = createBuffer(vkRendererBackend, bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    vkRendererBackend->mainDeletionQueue.pushFunction([=]() {
        destroyBuffer(vkRendererBackend, vkRendererBackend->gpuSceneDataBuffer);
    });

    // Allocate the descriptor slot from the heap and write the descriptor info onto it.
    vkRendererBackend->gpuSceneDataDescriptor = allocateResourceDescriptor(&vkRendererBackend->descriptorHeap, ResourceDescriptorKind::UniformBuffer);
    writeUniformBufferDescriptor(
        vkRendererBackend, 
        &vkRendererBackend->descriptorHeap, 
        vkRendererBackend->gpuSceneDataDescriptor,
        vkRendererBackend->gpuSceneDataBuffer,
        0,
        bufferSize
    );
}
