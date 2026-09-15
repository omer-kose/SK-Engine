#include <Application/Application.h>
#include <RendererBackend/Vulkan/VkRendererBackend.h>
#include <RendererBackend/Vulkan/VkSceneResources.h>
#include <RendererBackend/Vulkan/VkRenderContext.h>
#include <UI/UI.h>
#include <Scene/Scene.h>
#include <Renderer/GlobalGPUTypes.h>
#include <Renderer/RenderContext.h>
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
    SK::Scene::setGlobalLightingProperties(&scene, glm::vec4(0.1f), glm::vec4(glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)), 2.0f), glm::vec4(1.0f));
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
            SK::VkRendererBackend::handleWindowResize(&vkRendererBackend);
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
            SK::Renderer::updateSceneBuffer(&renderContext, scene.gpuSceneData);

            SK::ForwardRenderer::Input forwardInput{};
            forwardInput.drawContext = &scene.drawContext;
            SK::ForwardRenderer::draw(&renderContext, forwardRendererResources, forwardInput);

            SK::UI::draw(&vkRendererBackend);
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