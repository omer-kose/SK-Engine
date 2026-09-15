#include "Scene.h"

#include <cassert>
#include <string>

#include <glm/gtx/transform.hpp>

#include <AssetSystem/AssetImporter_GLTF.h>
#include <Renderer/DrawPacketBuilder.h>

#include "GLTFInstanceBuilder.h"

#include <fmt/format.h>

void SK::Scene::setCameraProperties(State* scene, const glm::vec3& position, float pitch, float yaw)
{
	scene->camera.velocity = glm::vec3(0.0f);
	scene->camera.position = position;
	scene->camera.pitch = pitch;
	scene->camera.yaw = yaw;
}

void SK::Scene::setProjectionProperties(State* scene, float fov, float nearPlane, float farPlane)
{
	scene->fov = fov;
	scene->nearPlane = nearPlane;
	scene->farPlane = farPlane;
}

void SK::Scene::setGlobalLightingProperties(State* scene, const glm::vec4& ambientColor, const glm::vec4& sunlightDirection, const glm::vec4& sunlightColor)
{
	scene->ambientColor = ambientColor;
	scene->sunlightDirection = sunlightDirection;
	scene->sunlightColor = sunlightColor;
}

void SK::Scene::updateGPUSceneData(State* scene, uint32_t viewportWidth, uint32_t viewportHeight)
{
	const float aspectRatio = static_cast<float>(viewportWidth) / viewportHeight;

	scene->gpuSceneData.view = scene->camera.getViewMatrix();
	scene->gpuSceneData.proj = glm::perspectiveRH_ZO(glm::radians(scene->fov), aspectRatio, scene->nearPlane, scene->farPlane);
	// Vulkan clip-space convention.
	scene->gpuSceneData.proj[1][1] *= -1.0f;
	scene->gpuSceneData.viewproj = scene->gpuSceneData.proj * scene->gpuSceneData.view;
	scene->gpuSceneData.camPos = glm::vec4(scene->camera.position, 1.0f);

	scene->gpuSceneData.ambientColor = scene->ambientColor;
	scene->gpuSceneData.sunlightDirection = scene->sunlightDirection;
	scene->gpuSceneData.sunlightColor = scene->sunlightColor;
}

void SK::Scene::updateCamera(State* scene)
{
	scene->camera.update();
}

bool SK::Scene::loadGLTFScene(State* scene, std::string_view filePath, const glm::mat4& sceneWorldTransform)
{
	SK::Asset::ImportedAsset importedAsset;
	if (!SK::Asset::importGLTF(filePath, &importedAsset))
	{
		fmt::println("The GLTF scene asset with path: {} could not be loaded.", filePath);
		return false;
	}

	if (!importedAsset.gltfScene.has_value())
	{
		fmt::println("The loaded GLTF scene asset with path: {} has no valid GLTF Scene.", filePath);
		return false;
	}

	// String copy as imported asset will be "moved" into the asset registry.
	const std::string sceneName = importedAsset.gltfScene.value().name;

	SK::Asset::registerImported(&scene->assetRegistry, &scene->materialRegistry, std::move(importedAsset));

	// A scene has only one GLTF Scene active for now, so directly build the mesh instances and the draw context.
	SK::Scene::buildMeshInstancesFromGLTFScene(&scene->assetRegistry, sceneName, sceneWorldTransform, scene->meshInstances);

	buildDrawContext(scene);

	return true;
}

void SK::Scene::buildDrawContext(State* scene)
{
	SK::Renderer::buildDrawPacketsFromMeshInstances(&scene->assetRegistry, &scene->materialRegistry, scene->meshInstances, &scene->drawContext);
}

void SK::Scene::clear(State* scene)
{
	scene->drawContext.clear();
	scene->meshInstances.clear();

	SK::Material::clearMaterialRegistry(&scene->materialRegistry);
	SK::Asset::clearAssetRegistry(&scene->assetRegistry);
}
