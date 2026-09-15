#version 450

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#include "input_structures.h"

layout (location = 0) in VSIn
{
	vec3 normal;
	vec4 tangent;
	vec3 worldPos;
	vec2 uv;
} vsIn;

layout (location = 0) out vec4 fragColor;

const float PI = 3.14159265359;

#define SAMPLER2D(tID, sID) sampler2D(textures[nonuniformEXT(tID)], samplers[sID])

vec3 fetchNormal(in PBRData pbrData)
{
	if(pbrData.normalTexture != INVALID_RESOURCE_DESCRIPTOR_HANDLE)
	{
		// Map from [0, 1] to [-1, 1]
		vec3 sampledNormal = texture(SAMPLER2D(pbrData.normalTexture, pbrData.normalTextureSampler), vsIn.uv).xyz * 2.0f - 1.0f;

		vec3 N = normalize(vsIn.normal);
		vec3 T = normalize(vsIn.tangent.xyz);
		T = normalize(T - dot(T, N) * N); // Gram-Schmidt re-orthogonalize
		vec3 B = cross(N, T) * vsIn.tangent.w; // By GLTF 2.0 spec: bitangent = cross(normal.xyz, tangent.xyz) * tangent.w
		mat3 TBN = mat3(T, B, N);
		return normalize(TBN * sampledNormal);
	}
	else
	{
		return normalize(vsIn.normal);
	}
}

/*
	D: Trowbridge-Reitz GGX normal distribution function

	Note that alpha = roughness squared.
*/
float distributionGGX(float NdotH, float alpha)
{
	float a2 = alpha * alpha;
	float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;

	return a2 / (PI * denom * denom);
}

// Schlick-GGX for one direction X
float geometrySchlickGGX(float NdotX, float k)
{
	return NdotX / (NdotX * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float k)
{
	return geometrySchlickGGX(NdotV, k) * geometrySchlickGGX(NdotL, k); // masking * shadowing
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 computeCookTorranceBRDF(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float roughness, float metallic, vec3 F0)
{
	vec3 H = normalize(V + L);
	float NdotV = max(dot(N, V), 1e-4); // floor this at 1e-4 not 0. NdotV can be pushed down to zero or below at grazing angles causing silhouette pixels to be blacked out. 
	float NdotL = max(dot(N, L), 0.0f);
	float NdotH = max(dot(N, H), 0.0f);
	float alpha = roughness * roughness;
	float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f; // k for direct lighting 

	float D = distributionGGX(NdotH, alpha);
	float G = geometrySmith(NdotV, NdotL, k);
	vec3 F = fresnelSchlick(max(dot(H, V), 0.0f), F0);
	vec3 specular = (D * G * F) / (4.0f * NdotV * NdotL + 0.0001f); // Cook-Torrance Specular term

	vec3 kD = (vec3(1.0f) - F) * (1.0f - metallic); // energy split; if pure metallic there will be no diffuse light
	vec3 diffuse = kD * albedo / PI; // Lambert
	return (diffuse + specular) * radiance * NdotL;  
}

void main() 
{
	PBRData pbrData = pbrMaterials[pushData.materialIndex];
	vec3 V = normalize(sceneData[pushData.frameIndex].camPos.xyz - vsIn.worldPos);
	vec3 N = fetchNormal(pbrData);
	// baseColor (albedo) is in sRGB format. Applying gamma corection to map it back to the linear space as we always work in linear space.
	vec3 albedo = pbrData.baseColorFactor.rgb;
	if(pbrData.baseColorTexture != INVALID_RESOURCE_DESCRIPTOR_HANDLE)
	{
		albedo *= pow(texture(SAMPLER2D(pbrData.baseColorTexture, pbrData.baseColorTextureSampler), vsIn.uv).rgb, vec3(2.2f));
	}
	// in GLTF 2.0 format, green channel contains roughness values and the blue channel contains metalness values
	float roughness = pbrData.roughnessFactor;
	float metallic = pbrData.metallicFactor;
	if(pbrData.metallicRoughnessTexture != INVALID_RESOURCE_DESCRIPTOR_HANDLE)
	{
		roughness *= texture(SAMPLER2D(pbrData.metallicRoughnessTexture, pbrData.metallicRoughnessTextureSampler), vsIn.uv).g;
		metallic *= texture(SAMPLER2D(pbrData.metallicRoughnessTexture, pbrData.metallicRoughnessTextureSampler), vsIn.uv).b;
	}
	vec3 F0 = mix(vec3(0.04f), albedo, metallic);

	vec3 Lo = vec3(0.0f);
	// There is only directional sunlight for now.
	vec3 L = -sceneData[pushData.frameIndex].sunlightDirection.xyz; // light direction is always passed normalized.
	Lo += computeCookTorranceBRDF(N, V, L, sceneData[pushData.frameIndex].sunlightColor.xyz * sceneData[pushData.frameIndex].sunlightDirection.w, albedo, roughness, metallic, F0);

	vec3 color = Lo;
	// Tonemap the result from LDR to HDR
	color = color / (color + 1.0f);
	// Apply gamma correction
	color = pow(color, vec3(1.0f / 2.2f));

	fragColor = vec4(color, 1.0f);
}