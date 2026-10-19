#pragma once

#include <Renderer/RenderContext.h>
#include <Renderer/DrawContext.h>

#include <glm/mat4x4.hpp>

namespace SK::ForwardRenderer
{
	struct Input
	{
		const SK::Renderer::DrawContext* drawContext = nullptr;
	};

	void init(SK::Renderer::RenderContext* renderContext);
	void draw(SK::Renderer::RenderContext* renderContext, const Input& input);
	void clear();
};