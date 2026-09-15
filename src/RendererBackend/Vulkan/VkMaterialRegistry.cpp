#include "VkMaterialRegistry.h"

#include <RendererBackend/Vulkan/VkAssetRegistry.h>
#include <RendererBackend/Vulkan/VkRendererBackend.h>

#include <AssetSystem/AssetRegistry.h>
#include <MaterialSystem/MaterialRegistry.h>

void SK::VkRendererBackend::buildMaterialRegistry(State* vkRendererBackend, SK::Asset::AssetRegistry* assetRegistry, SK::Material::MaterialRegistry* materialRegistry, VkAssetRegistry* vkAssetRegistry, VkMaterialRegistry* vkMaterialRegistry)
{
	std::vector<SK::Material::PBRData> pbrMaterialData;
	pbrMaterialData.reserve(materialRegistry->instances.size());

	// Gather up PBRData into a single buffer
	for (const auto& materialInstance : materialRegistry->instances)
	{
		SK::Material::PBRData gpuPbrMaterialData{};
		const SK::Material::PBRData* pbrData = &materialInstance.materialData;
		for (int i = 0; i < 4; ++i)
		{
			gpuPbrMaterialData.baseColorFactor[i] = pbrData->baseColorFactor[i];
		}

		gpuPbrMaterialData.metallicFactor = pbrData->metallicFactor;
		gpuPbrMaterialData.roughnessFactor = pbrData->roughnessFactor;

		// With descriptor heaps, descriptor handle indices into the resource heap are stored. Doing the mapping from texture index to descriptor handle index here.
		if (pbrData->baseColorTexture != SK::Material::INVALID_TEXTURE)
		{
			gpuPbrMaterialData.baseColorTexture = vkAssetRegistry->textures[pbrData->baseColorTexture].imageDescriptor.index;
			gpuPbrMaterialData.baseColorTextureSampler = vkAssetRegistry->textures[pbrData->baseColorTexture].samplerDescriptor.index;
		}
		
		if (pbrData->metallicRoughnessTexture != SK::Material::INVALID_TEXTURE)
		{
			gpuPbrMaterialData.metallicRoughnessTexture = vkAssetRegistry->textures[pbrData->metallicRoughnessTexture].imageDescriptor.index;
			gpuPbrMaterialData.metallicRoughnessTextureSampler = vkAssetRegistry->textures[pbrData->metallicRoughnessTexture].samplerDescriptor.index;
		}
		
		if (pbrData->normalTexture != SK::Material::INVALID_TEXTURE)
		{
			gpuPbrMaterialData.normalTexture = vkAssetRegistry->textures[pbrData->normalTexture].imageDescriptor.index;
			gpuPbrMaterialData.normalTextureSampler = vkAssetRegistry->textures[pbrData->normalTexture].samplerDescriptor.index;
		}

		if (pbrData->emissiveTexture != SK::Material::INVALID_TEXTURE)
		{
			gpuPbrMaterialData.emissiveTexture = vkAssetRegistry->textures[pbrData->emissiveTexture].imageDescriptor.index;
			gpuPbrMaterialData.emissiveTextureSampler = vkAssetRegistry->textures[pbrData->emissiveTexture].samplerDescriptor.index;
		}

		if (pbrData->occlusionTexture != SK::Material::INVALID_TEXTURE)
		{
			gpuPbrMaterialData.occlusionTexture = vkAssetRegistry->textures[pbrData->occlusionTexture].imageDescriptor.index;
			gpuPbrMaterialData.occlusionTextureSampler = vkAssetRegistry->textures[pbrData->occlusionTexture].samplerDescriptor.index;
		}
		
		pbrMaterialData.push_back(gpuPbrMaterialData);
	}

	// Create and upload the GPU buffer that will hold all the material data in the asset system.
	const size_t pbrMaterialBufferSize = pbrMaterialData.size() * sizeof(SK::Material::PBRData);
	assert(pbrMaterialBufferSize >= 0);
	vkMaterialRegistry->pbrMaterialBuffer = SK::VkRendererBackend::createAndUploadGPUBuffer(vkRendererBackend, pbrMaterialBufferSize,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, static_cast<void*>(pbrMaterialData.data()));

	// Create the descriptor
	vkMaterialRegistry->pbrMaterialBufferDescriptor = SK::VkRendererBackend::allocateResourceDescriptor(&vkRendererBackend->descriptorHeap, SK::VkRendererBackend::ResourceDescriptorKind::StorageBuffer);
	SK::VkRendererBackend::writeStorageBufferDescriptor(
		vkRendererBackend,
		&vkRendererBackend->descriptorHeap,
		vkMaterialRegistry->pbrMaterialBufferDescriptor,
		vkMaterialRegistry->pbrMaterialBuffer,
		0,
		pbrMaterialBufferSize
	);
}

void SK::VkRendererBackend::clearMaterialRegistry(State* vkRendererBackend, VkMaterialRegistry* vkMaterialRegistry)
{
	if (vkMaterialRegistry->pbrMaterialBuffer.buffer != VK_NULL_HANDLE)
	{
		destroyBuffer(vkRendererBackend, vkMaterialRegistry->pbrMaterialBuffer);
		vkMaterialRegistry->pbrMaterialBuffer = {};
	}
}
