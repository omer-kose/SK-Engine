#include "RenderContext.h"

#include <cassert>

SK::Renderer::PipelineHandle SK::Renderer::getGraphicsPipeline(RenderContext* renderContext, const GraphicsPipelineDesc& desc)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getGraphicsPipeline != nullptr);

	return renderContext->api->getGraphicsPipeline(renderContext, desc);
}

SK::Renderer::PipelineHandle SK::Renderer::getComputePipeline(RenderContext* renderContext, const ComputePipelineDesc& desc)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getComputePipeline != nullptr);

	return renderContext->api->getComputePipeline(renderContext, desc);
}

uint32_t SK::Renderer::getFrameNumber(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getFrameNumber != nullptr);

	return renderContext->api->getFrameNumber(renderContext);
}

uint8_t SK::Renderer::getFrameIndex(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getFrameIndex != nullptr);

	return renderContext->api->getFrameIndex(renderContext);
}

bool SK::Renderer::beginFrame(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->beginFrame != nullptr);

	return renderContext->api->beginFrame(renderContext);
}

void SK::Renderer::endFrame(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->endFrame != nullptr);

	renderContext->api->endFrame(renderContext);
}

void SK::Renderer::updateSceneBuffer(RenderContext* renderContext, const SK::Renderer::GPUSceneData& gpuSceneData)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->updateSceneBuffer != nullptr);

	renderContext->api->updateSceneBuffer(renderContext, gpuSceneData);
}

SK::Renderer::BufferDeviceAddress SK::Renderer::getVertexBufferDeviceAddress(RenderContext* renderContext, size_t meshIndex)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getVertexBufferDeviceAddress != nullptr);

	return renderContext->api->getVertexBufferDeviceAddress(renderContext, meshIndex);
}

SK::Renderer::BufferDeviceAddress SK::Renderer::getBufferDeviceAddress(RenderContext* renderContext, BufferHandle bufferHandle)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getBufferDeviceAddress != nullptr);

	return renderContext->api->getBufferDeviceAddress(renderContext, bufferHandle);
}

SK::Renderer::BufferHandle SK::Renderer::createBuffer(RenderContext* renderContext, const BufferDesc& desc)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->createBuffer != nullptr);
	
	return renderContext->api->createBuffer(renderContext, desc);
}

SK::Renderer::TextureHandle SK::Renderer::createTexture(RenderContext* renderContext, const TextureDesc& desc)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->createTexture != nullptr);

	return renderContext->api->createTexture(renderContext, desc);
}

void SK::Renderer::executeFrameGraphBarriers(RenderContext* renderContext, const FrameGraph& fg, const std::vector<FrameGraphBarrier>& barriers)
{
	assert(renderContext);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->executeFrameGraphBarriers != nullptr);

	renderContext->api->executeFrameGraphBarriers(renderContext, fg, barriers);
}

void SK::Renderer::beginMainRendering(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->beginMainRendering != nullptr);

	renderContext->api->beginMainRendering(renderContext);
}

void SK::Renderer::endRendering(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->endRendering != nullptr);

	renderContext->api->endRendering(renderContext);
}

void SK::Renderer::bindPipeline(RenderContext* renderContext, PipelineHandle pipeline)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->bindPipeline != nullptr);
	assert(pipeline.id != INVALID_HANDLE);

	renderContext->api->bindPipeline(renderContext, pipeline);
}

uint32_t SK::Renderer::getSceneDataDescriptorIndex(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getSceneDataDescriptorIndex != nullptr);

	return renderContext->api->getSceneDataDescriptorIndex(renderContext);
}

uint32_t SK::Renderer::getMaterialDataDescriptorIndex(RenderContext* renderContext)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->getMaterialDataDescriptorIndex != nullptr);

	return renderContext->api->getMaterialDataDescriptorIndex(renderContext);
}

void SK::Renderer::pushData(RenderContext* renderContext, uint32_t offset, uint32_t size, const void* data)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->pushData != nullptr);
	assert(data != nullptr || size == 0);

	renderContext->api->pushData(renderContext, offset, size, data);
}

void SK::Renderer::bindIndexBuffer(RenderContext* renderContext, size_t meshIndex, IndexType indexType)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->bindIndexBuffer != nullptr);

	renderContext->api->bindIndexBuffer(renderContext, meshIndex, indexType);
}

void SK::Renderer::drawIndexed(RenderContext* renderContext, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->drawIndexed != nullptr);

	renderContext->api->drawIndexed(renderContext, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void SK::Renderer::dispatch(RenderContext* renderContext, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	assert(renderContext != nullptr);
	assert(renderContext->api != nullptr);
	assert(renderContext->api->dispatch != nullptr);

	renderContext->api->dispatch(renderContext, groupCountX, groupCountY, groupCountZ);
}