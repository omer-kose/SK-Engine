
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types : require

#define INVALID_RESOURCE_DESCRIPTOR_HANDLE 0xffffffffu // UINT32_MAX

struct Vertex
{
	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 tangent;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer
{
	Vertex vertices[];
};

layout(push_constant, scalar) uniform PushData
{
	mat4 worldMatrix;
	VertexBuffer vertexBuffer;
	uint frameIndex;
	uint materialIndex; // Index into the PBRMaterialData buffer
} pushData;

struct SceneData
{
	mat4 view;
	mat4 proj;
	mat4 viewproj;
	vec4 camPos;
	vec4 ambientColor;
	vec4 sunlightDirection; //w for sun power
	vec4 sunlightColor;
};

layout(binding = 0) uniform SceneUBO
{
	SceneData sceneData[2]; // per frame-in-flight
}; 

struct PBRData
{
	vec4 baseColorFactor;
	float metallicFactor;
	float roughnessFactor;
	// Texture ids
	uint baseColorTexture;
	uint metallicRoughnessTexture;
	uint normalTexture;
	uint emissiveTexture;
	uint occlusionTexture;
	// Sampler ids
	uint8_t baseColorTextureSampler;
	uint8_t metallicRoughnessTextureSampler;
	uint8_t normalTextureSampler;
	uint8_t emissiveTextureSampler;
	uint8_t occlusionTextureSampler;
};

// Bindless Material + Texture resources
layout(binding = 1, scalar) readonly buffer PBRMaterials
{
	PBRData pbrMaterials[];
};

layout(binding = 2) uniform texture2D textures[];
layout(binding = 3) uniform sampler samplers[];