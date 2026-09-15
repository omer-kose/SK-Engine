#include "AssetRegistry.h"

#include <RendererBackend/Vulkan/VkRendererBackend.h>

void SK::Asset::registerImported(AssetRegistry* assetRegistry, SK::Material::MaterialRegistry* materialRegistry, ImportedAsset&& importedAsset)
{
    const uint32_t meshBaseIndex = static_cast<uint32_t>(assetRegistry->meshes.size());
    const uint32_t textureBaseIndex = static_cast<uint32_t>(assetRegistry->textures.size());
    const uint32_t materialBaseIndex = static_cast<uint32_t>(materialRegistry->instances.size());

    // Remap imported material texture indices (local -> global texture indices) and register to the MaterialRegistry
    for(auto& mat : importedAsset.materials)
    {
        mat.materialData.baseColorTexture += (mat.materialData.baseColorTexture != SK::Material::INVALID_TEXTURE) ? textureBaseIndex : 0;
        mat.materialData.metallicRoughnessTexture += (mat.materialData.metallicRoughnessTexture != SK::Material::INVALID_TEXTURE) ? textureBaseIndex : 0;
        mat.materialData.normalTexture += (mat.materialData.normalTexture != SK::Material::INVALID_TEXTURE) ? textureBaseIndex : 0;
        mat.materialData.emissiveTexture += (mat.materialData.emissiveTexture != SK::Material::INVALID_TEXTURE) ? textureBaseIndex : 0;
        mat.materialData.occlusionTexture += (mat.materialData.occlusionTexture != SK::Material::INVALID_TEXTURE) ? textureBaseIndex : 0;

        SK::Material::registerInstance(materialRegistry, std::move(mat));
    }

    // Remap submesh material indices (local -> global material indices) and register the meshes to the AssetRegistry
    for(auto& mesh : importedAsset.meshes)
    {
        for(auto& subMesh : mesh.subMeshes)
        {
            if(subMesh.materialIndex != SK::Material::INVALID_MATERIAL)
            {
                subMesh.materialIndex += materialBaseIndex;
            }
        }

        uint32_t idx = static_cast<uint32_t>(assetRegistry->meshes.size());
        assetRegistry->meshIndexByName[mesh.name] = idx;
        assetRegistry->meshes.push_back(std::move(mesh));
    }

    for(auto& tex : importedAsset.textures)
    {
        uint32_t idx = static_cast<uint32_t>(assetRegistry->textures.size());
        assetRegistry->textureIndexByName[tex.name] = idx;
        assetRegistry->textures.push_back(std::move(tex));
    }

    // Register GLTF scene (if any) and remap local mesh indices to global
    if(importedAsset.gltfScene.has_value())
    {
        GLTFScene& scene = importedAsset.gltfScene.value();

        for(auto& node : scene.nodes)
        {
            // Remap local mesh indices to global
            if(node.meshIndex >= 0)
            {
                node.meshIndex += static_cast<int>(meshBaseIndex);
            }
        }

        uint32_t idx = static_cast<uint32_t>(assetRegistry->gltfScenes.size());
        assetRegistry->gltfSceneIndexByName[scene.name] = idx;
        assetRegistry->gltfScenes.push_back(std::move(scene));
    }
}

void SK::Asset::discardCPUMeshData(AssetRegistry* assetRegistry)
{
    for(auto& mesh : assetRegistry->meshes)
    {
        if(mesh.retention == CPURetention::DropAfterUpload)
        {
            mesh.vertices.clear();
            mesh.indices.clear();
            mesh.vertices.shrink_to_fit();
            mesh.indices.shrink_to_fit();
        }
    }
}

void SK::Asset::discardCPUTextureData(AssetRegistry* assetRegistry)
{
    for(auto& tex : assetRegistry->textures)
    {
        if(tex.retention == CPURetention::DropAfterUpload)
        {
            tex.image.data.clear();
            tex.image.data.shrink_to_fit();
        }
    }
}

void SK::Asset::clearAssetRegistry(SK::Asset::AssetRegistry* assetRegistry)
{
    assetRegistry->meshes.clear();
    assetRegistry->textures.clear();
    assetRegistry->gltfScenes.clear();
    assetRegistry->meshIndexByName.clear();
    assetRegistry->textureIndexByName.clear();
    assetRegistry->gltfSceneIndexByName.clear();
}
