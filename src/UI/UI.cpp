#include "UI.h"

#include <RendererBackend/Vulkan/VkRendererBackend.h>
#include <RendererBackend/Vulkan/VkInitializers.h>
#include <RendererBackend/Vulkan/VkImages.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

#include "SDL_events.h"

void SK::UI::init(State* ui, SK::VkRendererBackend::State* vkRendererBackend)
{
    assert(ui->isInitialized == false);

    // 1: create descriptor pool for IMGUI
    // the size of the pool is very oversize, but it's copied from imgui demo  itself.
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .pNext = nullptr };
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;

    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(vkRendererBackend->device, &poolInfo, nullptr, &imguiPool));

    // 2. Initialize the ImGui Library
    // Initialize the core structures of ImGui
    ImGui::CreateContext();
    // Initialize ImGui for SDL
    ImGui_ImplSDL2_InitForVulkan(vkRendererBackend->window);
    // Initialize ImGui for Vulkan
    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = vkRendererBackend->instance;
    initInfo.PhysicalDevice = vkRendererBackend->chosenGPU;
    initInfo.Device = vkRendererBackend->device;
    initInfo.Queue = vkRendererBackend->graphicsQueue;
    initInfo.DescriptorPool = imguiPool;
    initInfo.MinImageCount = 3;
    initInfo.ImageCount = 3;
    initInfo.UseDynamicRendering = true;

    // Dynamic rendering parameters for ImGui to use
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = nullptr };
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vkRendererBackend->swapchainImageFormat;
   
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);

    // UI will destroy its own resources
    ui->deletionQueue.pushFunction([=]() {
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(vkRendererBackend->device, imguiPool, nullptr);
    });

    ui->isInitialized = true;
}

void SK::UI::processSDLEvents(const SDL_Event& e)
{
    ImGui_ImplSDL2_ProcessEvent(&e);
}

void SK::UI::beginFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void SK::UI::endFrame()
{
    // Make ImGui calculate internal draw structures
    ImGui::Render();
}

void SK::UI::draw(SK::VkRendererBackend::State* vkRendererBackend)
{
    VkCommandBuffer cmd = vkRendererBackend->currentCmdBuffer;
    uint32_t swapchainImageIndex = vkRendererBackend->currentSwapchainImageIndex;

    VkRenderingAttachmentInfo colorAttachment = SK::VkInit::attachmentInfo(vkRendererBackend->swapchainImageViews[swapchainImageIndex], nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = SK::VkInit::renderingInfo(vkRendererBackend->swapchainExtent, &colorAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

void SK::UI::shutdown(State* ui)
{
    if(ui->isInitialized)
    {
        ui->deletionQueue.flush();
        ui->isInitialized = true;
    }
}
