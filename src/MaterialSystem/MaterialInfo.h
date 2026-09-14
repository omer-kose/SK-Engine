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
	struct PBRData
	{
		float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float metallicFactor;
		float roughnessFactor;
		// Texture ids (On CPU these are actual indices into the textures array. On GPU these are descriptor handle indices into the resource heap.)
		uint32_t baseColorTexture;
		uint32_t metallicRoughnessTexture;
		uint32_t normalTexture;
		uint32_t emissiveTexture;
		// Sampler ids
		uint8_t baseColorTextureSampler;
		uint8_t metallicRoughnessTextureSampler;
		uint8_t normalTextureSampler;
		uint8_t emissiveTextureSampler;
	};

	struct Instance
	{
		// Defaulted to Opaque PBR Material
		AlphaMode alphaMode = AlphaMode::Opaque;

		PBRData materialData = PBRData{};
	};
}