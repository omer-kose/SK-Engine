#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include <Renderer/GlobalGPUTypes.h>

/*
	Asset System is API-agnostic. It only holds raw data + metadata. It follows retention policy. Unnecessary CPU data will be discarded after uploaded to the GPU.
*/

namespace SK::Asset
{
	enum class CPURetention : uint8_t
	{
		DropAfterUpload = 0,
		KeepOnCPU
	};

	enum class TextureFilter : uint8_t
	{
		NEAREST = 0,
		LINEAR
	};

	enum class TextureMipmapMode : uint8_t
	{
		NEAREST = 0,
		LINEAR
	};

	enum class TextureAddressMode : uint8_t
	{
		REPEAT = 0, // wrap
		CLAMP_TO_EDGE = 1
	};

	enum class TextureFormat : uint8_t
	{
		Undefined = 0,
		RGBA8_UNORM, // linear texture space for textures: metallic-roughness, normal, occlusion.
		RGBA8_SRGB // sRGB space for textures: baseColor (albedo), emissive. 
	};

	struct RawImage
	{
		std::vector<uint8_t> data;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t channels = 0;
	};

	struct TextureDescription
	{
		bool mipmapped = false;
		TextureFilter minFilter = TextureFilter::LINEAR;
		TextureFilter magFilter = TextureFilter::LINEAR;
		TextureMipmapMode mipmapMode = TextureMipmapMode::LINEAR;
		TextureAddressMode addressMode = TextureAddressMode::REPEAT;
		TextureFormat format = TextureFormat::Undefined; // Format must be provided.
	};

	struct RawTexture
	{
		RawImage image;
		TextureDescription description;
		CPURetention retention = CPURetention::DropAfterUpload;
		std::string name;
	};

	// Bounds of a mesh geometry. It both stores radius and extents. So, depending on the situation, a bounding box or a bounding sphere can be used
	struct MeshBounds
	{
		glm::vec3 origin; // origin of the bound
		float sphereRadius; // radius of the sphere
		glm::vec3 extents; // half edge lengths of the bounding box 
	};

	struct SubMesh
	{
		// starting index and count into the index buffer
		uint32_t startIndex = 0;
		uint32_t indexCount = 0;
		MeshBounds bounds;
		uint32_t materialIndex = UINT32_MAX;
	};

	struct RawMesh
	{
		std::vector<SK::Renderer::Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<SubMesh> subMeshes;
		CPURetention retention = CPURetention::DropAfterUpload;
		std::string name;
	};
}