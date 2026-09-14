#pragma once

#include <Renderer/RenderContext.h>
#include <Renderer/DrawContext.h>

#include <glm/mat4x4.hpp>

namespace SK::ForwardRenderer
{
	struct Resources
	{
		SK::Renderer::PipelineHandle opaquePipeline;
		SK::Renderer::PipelineHandle transparentPipeline;
	};

	struct Input
	{
		const SK::Renderer::DrawContext* drawContext = nullptr;
	};

	// Push data for mesh draws
	struct PushData
	{
		glm::mat4 worldMatrix;
		SK::Renderer::BufferDeviceAddress vertexBufferAddress;
		uint32_t frameIndex;
		uint32_t materialIndex;
	};

	void createResources(SK::Renderer::RenderContext* renderContext, Resources* resources);
	void draw(SK::Renderer::RenderContext* renderContext, const Resources& resources, const Input& input);
};