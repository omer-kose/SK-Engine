#version 450

#extension GL_GOOGLE_include_directive : require

#include "input_structures.h"

layout (location = 0) out VSOut
{
	vec3 normal;
	vec4 tangent;
	vec3 worldPos;
	vec2 uv;
} vsOut;

void main()
{
	Vertex v = pushData.vertexBuffer.vertices[gl_VertexIndex];
	vec4 worldPos = pushData.worldMatrix * vec4(v.position, 1.0f);
	mat3 normalTransformation =  mat3(inverse(transpose(pushData.worldMatrix))); // TODO: Pass the inverse transpose from the CPU side don't recompute it per vertex.
	vsOut.worldPos = vec3(worldPos);
	vsOut.normal = normalTransformation * v.normal;
	vsOut.tangent = vec4(normalTransformation * v.tangent.xyz, v.tangent.w);
	vsOut.uv = vec2(v.uv_x, v.uv_y);
	gl_Position = sceneData[pushData.frameIndex].viewproj * worldPos;
}