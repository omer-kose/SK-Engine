#include <Application/Application.h>
#include <RendererBackend/Vulkan/VkRendererBackend.h>
#include <RendererBackend/Vulkan/VkSceneResources.h>
#include <RendererBackend/Vulkan/VkRenderContext.h>
#include <UI/UI.h>
#include <Scene/Scene.h>
#include <Renderer/GlobalGPUTypes.h>
#include <Renderer/RenderContext.h>
#include <Renderer/FrameGraph.h>
#include <Renderer/ForwardRenderer.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

#include <thread>
#include <chrono>

// Program specific Event Context
struct EventContext
{
    SK::Scene::State* scene = nullptr;
};

// Event callback that will be called by the Application Layer.
static void SDLEventCallback(const SDL_Event& event, void* eventContext)
{
    auto* context = static_cast<EventContext*>(eventContext);

    if (context && context->scene)
    {
        SDL_Event mutableEvent = event;
        context->scene->camera.processSDLEvent(mutableEvent);
    }

    SK::UI::processSDLEvents(event);
}

int main(int argc, char* argv[])
{
	SK::Application::State application;
	SK::Application::init(&application, 1920, 1080);

	SK::VkRendererBackend::State vkRendererBackend;
	SK::VkRendererBackend::init(&vkRendererBackend, application.window, application.windowWidth, application.windowHeight);

    SK::UI::State ui;
    SK::UI::init(&ui, &vkRendererBackend);

    SK::Scene::State scene;
    SK::Scene::setCameraProperties(&scene, glm::vec3(0.0f, 2.0f, 0.0f), 0.0f, 90.0f);
    SK::Scene::setProjectionProperties(&scene, 70.0f, 0.1f, 10000.0f);
    SK::Scene::setGlobalLightingProperties(&scene, glm::vec4(0.1f), glm::vec4(glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)), 5.0f), glm::vec4(1.0f));
    const bool sceneLoaded = SK::Scene::loadGLTFScene(&scene, "../../assets/Sponza/Sponza.gltf");
    assert(sceneLoaded);

    SK::VkRendererBackend::VkSceneResources vkSceneResources;
    SK::VkRendererBackend::uploadSceneResources(&vkRendererBackend, &scene, &vkSceneResources);
    SK::Asset::discardCPUMeshData(&scene.assetRegistry);
    SK::Asset::discardCPUTextureData(&scene.assetRegistry);

    SK::VkRendererBackend::VkRenderContext vkRenderContext;
    SK::VkRendererBackend::initVkRenderContext(&vkRenderContext, &vkRendererBackend, &vkSceneResources);

    SK::Renderer::RenderContext renderContext = SK::VkRendererBackend::makeRenderContext(&vkRenderContext);

    EventContext eventContext{};
    eventContext.scene = &scene;

    // Renderer frontends
    SK::ForwardRenderer::Resources forwardRendererResources;
    SK::ForwardRenderer::createResources(&renderContext, &forwardRendererResources);

    // Until I have a rendering orchestrator, these will be here.
    const SK::Renderer::TextureHandle swapchainImageHandle = SK::Renderer::getSwapchainImageHandle(&renderContext);
    const SK::Renderer::TextureHandle mainDrawImageHandle = SK::Renderer::getMainDrawImageHandle(&renderContext);
    const SK::Renderer::TextureHandle mainDepthImageHandle = SK::Renderer::getMainDepthImageHandle(&renderContext);

    // main loop
    while(!application.shouldQuit)
    {
        // Begin frame time clock
        auto start = std::chrono::system_clock::now();

        SK::Application::handleSDLEvents(&application, &SDLEventCallback, &eventContext);

        // sleep if windows is minimized
        if(application.isMinimized)
        {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // RendererBackend checks for a resize requirement every frame internally
        if(vkRendererBackend.windowResizeRequested)
        {
            SK::Renderer::handleWindowResize(&renderContext);
        }

        // --- UI FRAME BEGIN ---
        SK::UI::beginFrame();

        // --- PROGRAM SPECIFIC UI CODE ---
        ImGui::Begin("Stats");
        ImGui::Text("frametime %f ms", vkRendererBackend.stats.frameTime);
        ImGui::Text("geometry draw recording time %f ms", vkRendererBackend.stats.geometryDrawRecordTime);
        ImGui::Text("update time %f ms", vkRendererBackend.stats.sceneUpdateTime);
        ImGui::Text("triangles %i", vkRendererBackend.stats.triangleCount);
        ImGui::Text("draws %i", vkRendererBackend.stats.drawCallCount);
        ImGui::End();

        // --- UI FRAME END ---
        SK::UI::endFrame();

        SK::Scene::updateCamera(&scene);
        SK::Scene::updateGPUSceneData(&scene, vkRendererBackend.windowExtent.width, vkRendererBackend.windowExtent.height);

        if(SK::Renderer::beginFrame(&renderContext))
        {
            SK::Renderer::updateBackendInternalImageInfos(&renderContext);
            SK::Renderer::updateSceneBuffer(&renderContext, scene.gpuSceneData);

            SK::Renderer::FrameGraph frameGraph;

            SK::Renderer::FrameGraphResourceHandle fgSwapchainResourceHandle = frameGraph.importTexture(
                swapchainImageHandle,
                SK::Renderer::getTextureDesc(&renderContext, swapchainImageHandle),
                SK::Renderer::FrameGraphResourceState::Undefined,
                SK::Renderer::FrameGraphResourceState::Present,
                "Swapchain Image"
            );

            SK::Renderer::FrameGraphResourceHandle fgMainDrawImageResourceHandle = frameGraph.importTexture(
                mainDrawImageHandle,
                SK::Renderer::getTextureDesc(&renderContext, mainDrawImageHandle),
                SK::Renderer::FrameGraphResourceState::Undefined,
                std::nullopt,
                "Main Draw Image"
            );

            SK::Renderer::FrameGraphResourceHandle fgMainDepthImageResourceHandle = frameGraph.importTexture(
                mainDepthImageHandle,
                SK::Renderer::getTextureDesc(&renderContext, mainDepthImageHandle),
                SK::Renderer::FrameGraphResourceState::Undefined,
                std::nullopt,
                "Main Depth Image"
            );

            frameGraph.addPass("Screen Clear Pass",
                [&](SK::Renderer::FrameGraphPassBuilder& builder) {
                    builder.writeTexture(fgMainDrawImageResourceHandle, SK::Renderer::FrameGraphResourceState::ColorAttachment, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                    builder.writeTexture(fgMainDepthImageResourceHandle, SK::Renderer::FrameGraphResourceState::DepthStencilAttachment, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                },
                [&](const SK::Renderer::FrameGraphPassContext& context) {
                    const SK::Renderer::TextureHandle mainDrawImage = context.getTexture(fgMainDrawImageResourceHandle);
                    const SK::Renderer::TextureHandle mainDepthImage = context.getTexture(fgMainDepthImageResourceHandle);
                    SK::Renderer::ClearValue mainDrawImageClearValue = SK::Renderer::ClearValue{ .color = { 0.0f, 0.0f, 0.0f, 1.0f } };
                    SK::Renderer::ClearValue mainDepthImageClearValue = SK::Renderer::ClearValue{ .depthStencil = {.depth = 1.0f } };

                    SK::Renderer::beginRendering(&renderContext, &mainDrawImage, &mainDrawImageClearValue, &mainDepthImage, &mainDepthImageClearValue);
                    SK::Renderer::endRendering(&renderContext);
                }
            );

            frameGraph.addPass("Forward Rendering",
                [&](SK::Renderer::FrameGraphPassBuilder& builder) {
                    builder.writeTexture(fgMainDrawImageResourceHandle, SK::Renderer::FrameGraphResourceState::ColorAttachment, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                    builder.writeTexture(fgMainDepthImageResourceHandle, SK::Renderer::FrameGraphResourceState::DepthStencilAttachment, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                },
                [&](const SK::Renderer::FrameGraphPassContext& context) {
                    SK::ForwardRenderer::Input forwardInput{};
                    forwardInput.drawContext = &scene.drawContext;
                    SK::ForwardRenderer::draw(context.renderContext, forwardRendererResources, forwardInput);
                }
            );

            frameGraph.addPass("Copy Draw to Swapchain",
                [&](SK::Renderer::FrameGraphPassBuilder& builder) {
                    builder.readTexture(fgMainDrawImageResourceHandle, SK::Renderer::FrameGraphResourceState::TransferRead, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                    builder.writeTexture(fgSwapchainResourceHandle, SK::Renderer::FrameGraphResourceState::TransferWrite, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                },
                [&](const SK::Renderer::FrameGraphPassContext& context) {
                    const SK::Renderer::TextureHandle& drawImage = context.getTexture(fgMainDrawImageResourceHandle);
                    const SK::Renderer::TextureHandle& swapchainImage = context.getTexture(fgSwapchainResourceHandle);

                    SK::Renderer::blitImage(&renderContext, drawImage, swapchainImage);
                }
            );

            frameGraph.addPass("UI Pass",
                [&](SK::Renderer::FrameGraphPassBuilder& builder) {
                    builder.readTexture(fgSwapchainResourceHandle, SK::Renderer::FrameGraphResourceState::ColorAttachment, SK::Renderer::toShaderStageFlags(SK::Renderer::ShaderStageFlagBits::None));
                    builder.setSideEffect();
                },
                [&](const SK::Renderer::FrameGraphPassContext& context) {
                    SK::UI::draw(&vkRendererBackend);
                }
            );

            if (frameGraph.compile(&renderContext))
            {
                frameGraph.execute(&renderContext);
            }

            SK::Renderer::endFrame(&renderContext);
        }

        auto end = std::chrono::system_clock::now();
        // Convert to microseconds (integer), then come back to miliseconds
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        vkRendererBackend.stats.frameTime = elapsed.count() / 1000.0f;
    }

    // Make sure that GPU finished executing every command before shutting down the systems.
    vkDeviceWaitIdle(vkRendererBackend.device);

    SK::VkRendererBackend::clearVkRenderContext(&vkRenderContext);
    SK::VkRendererBackend::clearSceneResources(&vkRendererBackend, &vkSceneResources);
    
    SK::Scene::clear(&scene);

    SK::UI::shutdown(&ui);

    SK::VkRendererBackend::shutdown(&vkRendererBackend);

	SK::Application::shutdown(&application);

	return 0;
}