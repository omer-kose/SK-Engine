#pragma once

#include <array>
#include <cstdint>
#include <variant>

/*
	For now, only PBR materials are supported.
*/

namespace SK::Material
{
	enum class AlphaMode : uint8_t
	{
		Opaque = 0,
		Transparent
	};

	static constexpr uint32_t INVALID_TEXTURE = UINT32_MAX;
	static constexpr uint32_t INVALID_MATERIAL = UINT32_MAX;

	// Using scalar layout for the material buffer. 1-to-1 matching with what will be stored on the GPU side.
	// The GLTF assets that will be used with this engine must have base color (albedo) and metallic roughness textures. Other 3 textures are optional.
	struct PBRData
	{
		// default values of factors are coming from GLTF 2.0 spec.
		float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
		// Texture ids (On CPU these are actual indices into the textures array. On GPU these are descriptor handle indices into the resource heap.)
		// Both texture ids and resource descriptor heap handles use UINT32_MAX for invalid handle.
		uint32_t baseColorTexture = INVALID_TEXTURE;
		uint32_t metallicRoughnessTexture = INVALID_TEXTURE;
		uint32_t normalTexture = INVALID_TEXTURE;
		uint32_t emissiveTexture = INVALID_TEXTURE;
		uint32_t occlusionTexture = INVALID_TEXTURE;
		// Sampler ids. Default values of the samplers doesn't matter as they are only used if the textures are valid.
		uint8_t baseColorTextureSampler;
		uint8_t metallicRoughnessTextureSampler;
		uint8_t normalTextureSampler;
		uint8_t emissiveTextureSampler;
		uint8_t occlusionTextureSampler;
	};

	struct Instance
	{
		// Defaulted to Opaque PBR Material
		AlphaMode alphaMode = AlphaMode::Opaque;

		PBRData materialData = PBRData{};
	};
}