#pragma once

#include <Renderer/RenderContext.h>

#include <RendererBackend/Vulkan/VkTypes.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SK::VkRendererBackend
{
	struct State;
	struct VkSceneResources;

	struct PipelineRecord
	{
		VkPipeline pipeline = VK_NULL_HANDLE;
		SK::Renderer::PipelineKind kind = SK::Renderer::PipelineKind::Graphics;
	};

	struct BufferRecord
	{
		SK::Renderer::BufferDesc desc{}; // cached
		AllocatedBuffer buffer;
		const char* debugName = nullptr;
	};

	struct TextureRecord
	{
		SK::Renderer::TextureDesc desc{}; // cached
		AllocatedImage image;
		uint8_t samplerIndex; // descriptor index of the sampler
		const char* debugName = nullptr;
	};

	/*
		VkRenderContext provides a context for the backend. The engine frontend (RenderContext) will use the functionality provided by the backend via VkRenderContext bridge.
	*/
	struct VkRenderContext
	{
		State* vkRendererBackend = nullptr;
		VkSceneResources* sceneResources = nullptr;

		// VkRenderContext does not own actual handles. It just caches the handles for functionality. The creation and cleaning of the handles are always done by the Renderer Backend.
		std::vector<PipelineRecord> pipelines;
		// TODO: This is for reusing the pipelines while retrieving them in frontend renderers. However, the retrievers also store those pipeline handles so this might be an unnecessary book-keeping.
		std::unordered_map<size_t, uint32_t> pipelineIndexByHash;

		std::vector<BufferRecord> buffers;
		std::vector<TextureRecord> textures;

		/*
			Backend owned images that are handled internally by the backend. VkRenderContext will also create a record and store handles for them and expose them via RenderContext but won't allocate or destroy them.
			Those backend owned images are created implicitly during initVkRenderContext, so their handles should be stored explicitly to be able to reach them inside the records.

			Even though there are multiple swapchain images and draw/depth images per frame-in-flight, having one single handle is sufficient provided that the required information kept updated.
			Just after every frame begin, the information related to current swapchain, draw/depth images will be updated always. All the images are identical to each other, so a single description shared by them all
			is sufficient.
		*/
		SK::Renderer::TextureHandle swapchainImageHandle;
		SK::Renderer::TextureHandle mainDrawImageHandle;
		SK::Renderer::TextureHandle mainDepthImageHandle;
	};

	void initVkRenderContext(VkRenderContext* vkRenderContext, State* vkRendererBackend, VkSceneResources* vkSceneResources);
	SK::Renderer::RenderContext makeRenderContext(VkRenderContext* vkRenderContext);
	void clearVkRenderContext(VkRenderContext* vkRenderContext);
}