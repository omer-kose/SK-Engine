#pragma once

#include <vector>
#include <unordered_map>
#include <string>

#include <RendererBackend/Vulkan/VkCommon.h>
#include <RendererBackend/Vulkan/VkDescriptorHeap.h>

namespace SK::Asset
{
	struct AssetRegistry;
}

namespace SK::VkRendererBackend
{
	struct State;

	struct VkAssetRegistry
	{
		struct GPUMesh
		{
			VkGPUMeshBuffers meshBuffers;
			std::string name;
		};

		struct GPUTexture
		{
			AllocatedImage image;
			ResourceDescriptorHandle imageDescriptor;
			SamplerDescriptorHandle samplerDescriptor;
			std::string name;
			bool ownsImage; // false when fallback error image is used in the absence of image data
		};

		// This is a 1-to-1 mapping to Asset Registry. This is ensured while building GPU assets via VkAssetBuilder. This exact mapping is utilized for efficient indexing in runtime hot draw path.
		std::vector<GPUMesh> meshes;
		std::vector<GPUTexture> textures;

		// Only for debug purposes
		std::unordered_map<std::string, uint32_t> meshIndexByName;
		std::unordered_map<std::string, uint32_t> textureIndexByName;
	};

	void buildGPUAssets(State* vkRendererBackend, SK::Asset::AssetRegistry* assetRegistry, VkAssetRegistry* vkAssetRegistry);
	void clearGPUAssets(State* vkRendererBackend, VkAssetRegistry* vkAssetRegistry);
}