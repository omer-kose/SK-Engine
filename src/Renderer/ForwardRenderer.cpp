#include "ForwardRenderer.h"

#include <Renderer/DrawContext.h>

#include <chrono>
#include <algorithm>

void SK::ForwardRenderer::createResources(SK::Renderer::RenderContext* renderContext, Resources* resources)
{
	SK::Renderer::GraphicsPipelineDesc opaqueDesc{};
	opaqueDesc.debugName = "Forward Opaque";
	opaqueDesc.shaders = {
		{ "../../shaders/glsl/forward/forward_vert.spv", SK::Renderer::ShaderStageFlagBits::VertexShader },
		{ "../../shaders/glsl/forward/forward_frag.spv", SK::Renderer::ShaderStageFlagBits::FragmentShader }
	};
	opaqueDesc.topology = SK::Renderer::PrimitiveTopology::TriangleList;
	opaqueDesc.polygonMode = SK::Renderer::PolygonMode::Fill;
	opaqueDesc.cullMode = SK::Renderer::CullMode::Back;
	opaqueDesc.frontFace = SK::Renderer::FrontFace::CounterClockwise;
	opaqueDesc.depthTest = true;
	opaqueDesc.depthWrite = true;
	opaqueDesc.depthCompare = SK::Renderer::CompareOp::LessOrEqual;
	opaqueDesc.blending = false;
	opaqueDesc.shaderResourceMappings = {
		{ 0, SK::Renderer::ShaderResourceType::UniformBuffer, SK::Renderer::getSceneDataDescriptorIndex(renderContext) },
		{ 1, SK::Renderer::ShaderResourceType::ReadOnlyStorageBuffer, SK::Renderer::getMaterialDataDescriptorIndex(renderContext) },
		// textures and samplers are accessed via the material buffer in the shaders, so their descriptorIndex field left 0.
		{ 2, SK::Renderer::ShaderResourceType::SampledImage, 0 },
		{ 3, SK::Renderer::ShaderResourceType::Sampler, 0 }
	};

	resources->opaquePipeline = SK::Renderer::getGraphicsPipeline(renderContext, opaqueDesc);

	SK::Renderer::GraphicsPipelineDesc transparentDesc = opaqueDesc;
	transparentDesc.debugName = "Forward Transparent";
	transparentDesc.depthTest = false;
	transparentDesc.depthWrite = false;
	transparentDesc.blending = true;

	resources->transparentPipeline = SK::Renderer::getGraphicsPipeline(renderContext, transparentDesc);
}

void SK::ForwardRenderer::draw(SK::Renderer::RenderContext* renderContext, const Resources& resources, const Input& input)
{
	SK::Renderer::beginMainRendering(renderContext);

	std::vector<uint32_t> opaqueDraws;
	opaqueDraws.reserve(input.drawContext->opaque.size());

	for (uint32_t i = 0; i < input.drawContext->opaque.size(); ++i)
	{
		opaqueDraws.push_back(i);
	}

	std::sort(opaqueDraws.begin(), opaqueDraws.end(), [&](uint32_t iA, uint32_t iB) {
		const SK::Renderer::DrawPacket& a = input.drawContext->opaque[iA];
		const SK::Renderer::DrawPacket& b = input.drawContext->opaque[iB];

		return a.meshIndex < b.meshIndex;
		});

	uint32_t lastMeshIndex = UINT32_MAX;

	auto drawPacket = [&](const SK::Renderer::DrawPacket& packet) {
		PushData pushConstants{};
		pushConstants.worldMatrix = packet.worldTransform;
		pushConstants.vertexBufferAddress = SK::Renderer::getVertexBufferDeviceAddress(renderContext, packet.meshIndex);
		pushConstants.frameIndex = SK::Renderer::getFrameIndex(renderContext);
		pushConstants.materialIndex = packet.materialIndex;

		SK::Renderer::pushData(renderContext, 0, sizeof(PushData), &pushConstants);

		if (lastMeshIndex != packet.meshIndex)
		{
			lastMeshIndex = packet.meshIndex;
			SK::Renderer::bindIndexBuffer(renderContext, packet.meshIndex, SK::Renderer::IndexType::Uint32);
		}

		SK::Renderer::drawIndexed(renderContext, packet.indexCount, 1, packet.startIndex, 0, 0);
	};

	SK::Renderer::bindPipeline(renderContext, resources.opaquePipeline);

	for (uint32_t drawIndex : opaqueDraws)
	{
		drawPacket(input.drawContext->opaque[drawIndex]);
	}

	SK::Renderer::bindPipeline(renderContext, resources.transparentPipeline);

	for (const SK::Renderer::DrawPacket& packet : input.drawContext->transparent)
	{
		drawPacket(packet);
	}

	SK::Renderer::endRendering(renderContext);
}
