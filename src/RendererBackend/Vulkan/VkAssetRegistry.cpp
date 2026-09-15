#include "VkAssetRegistry.h"

#include <RendererBackend/Vulkan/VkRendererBackend.h>
#include <AssetSystem/AssetRegistry.h>

#include <RendererBackend/Vulkan/VkInitializers.h>

static VkFilter mapFilterMode(SK::Asset::TextureFilter textureFilter)
{
    switch (textureFilter)
    {
    case SK::Asset::TextureFilter::NEAREST:
        return VK_FILTER_NEAREST;
    case SK::Asset::TextureFilter::LINEAR:
        return VK_FILTER_LINEAR;
    default:
        return VK_FILTER_LINEAR;
    }
}

static VkSamplerMipmapMode mapMipmapMode(SK::Asset::TextureMipmapMode mipmapMode)
{
    switch (mipmapMode)
    {
    case SK::Asset::TextureMipmapMode::NEAREST:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    case SK::Asset::TextureMipmapMode::LINEAR:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    default:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
}

static VkSamplerAddressMode mapAddressMode(SK::Asset::TextureAddressMode addressMode)
{
    switch (addressMode)
    {
    case SK::Asset::TextureAddressMode::REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case SK::Asset::TextureAddressMode::CLAMP_TO_EDGE:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkFormat mapTextureFormat(SK::Asset::TextureFormat format)
{
    switch (format)
    {
    case SK::Asset::TextureFormat::RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case SK::Asset::TextureFormat::RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

void SK::VkRendererBackend::buildGPUAssets(State* vkRendererBackend, SK::Asset::AssetRegistry* assetRegistry, VkAssetRegistry* vkAssetRegistry)
{
    // Meshes
    vkAssetRegistry->meshes.reserve(assetRegistry->meshes.size());
    for (auto& mesh : assetRegistry->meshes)
    {
        VkAssetRegistry::GPUMesh gpuMesh{};
        gpuMesh.name = mesh.name;

        gpuMesh.meshBuffers = SK::VkRendererBackend::uploadMesh(vkRendererBackend, mesh.vertices, mesh.indices);

        uint32_t idx = static_cast<uint32_t>(vkAssetRegistry->meshes.size());
        vkAssetRegistry->meshIndexByName[gpuMesh.name] = idx;
        vkAssetRegistry->meshes.push_back(std::move(gpuMesh));
    }

    // Textures
    vkAssetRegistry->textures.reserve(assetRegistry->meshes.size());
    for (const auto& texture : assetRegistry->textures)
    {
        VkAssetRegistry::GPUTexture gpuTexture{};
        gpuTexture.name = texture.name;

        // Create image from tex.image.data
        if (!texture.image.data.empty())
        {
            VkFormat imageFormat = mapTextureFormat(texture.description.format);
            assert(imageFormat != VK_FORMAT_UNDEFINED);
            // Currently, all the loaded textures from GLTF assets are in 32 bit (8 per channel) format. Even if the alpha channel is not used (like normal maps), that channel is padded during asset loading.
            size_t dataSize = texture.image.width * texture.image.height * 1 * 4;
            gpuTexture.image = SK::VkRendererBackend::createImage(vkRendererBackend, (void*)texture.image.data.data(), dataSize, VkExtent3D{ texture.image.width, texture.image.height, 1 }, imageFormat, VK_IMAGE_USAGE_SAMPLED_BIT, texture.description.mipmapped);
            gpuTexture.ownsImage = true;

            // Create the image descriptor
            gpuTexture.imageDescriptor = SK::VkRendererBackend::allocateResourceDescriptor(&vkRendererBackend->descriptorHeap, SK::VkRendererBackend::ResourceDescriptorKind::SampledImage);
            SK::VkRendererBackend::writeSampledImageDescriptor(
                vkRendererBackend,
                &vkRendererBackend->descriptorHeap,
                gpuTexture.imageDescriptor,
                SK::VkRendererBackend::createImageViewInfo(vkRendererBackend, gpuTexture.image),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL // sampled image
            );
        }
        else
        {
            // Absence of the image data is actually a bug for a texture. Assign error image for debugging.
            gpuTexture.image = vkRendererBackend->errorCheckerboardImage;
            gpuTexture.ownsImage = false;

            // Still create an image descriptor (leads to duplicate descriptors if more than one errorenous textures are loaded but it is fine as errorenous textures are there to be cleaned not used in a running program).
            gpuTexture.imageDescriptor = SK::VkRendererBackend::allocateResourceDescriptor(&vkRendererBackend->descriptorHeap, SK::VkRendererBackend::ResourceDescriptorKind::SampledImage);
            SK::VkRendererBackend::writeSampledImageDescriptor(
                vkRendererBackend,
                &vkRendererBackend->descriptorHeap,
                gpuTexture.imageDescriptor,
                SK::VkRendererBackend::createImageViewInfo(vkRendererBackend, gpuTexture.image),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL // sampled image
            );
        }

        gpuTexture.samplerDescriptor = SK::VkRendererBackend::createSamplerDescriptor(vkRendererBackend,
            mapFilterMode(texture.description.minFilter),
            mapFilterMode(texture.description.magFilter),
            mapMipmapMode(texture.description.mipmapMode),
            mapAddressMode(texture.description.addressMode)
        );

        uint32_t idx = static_cast<uint32_t>(vkAssetRegistry->textures.size());
        vkAssetRegistry->textureIndexByName[gpuTexture.name] = idx;
        vkAssetRegistry->textures.push_back(std::move(gpuTexture));
    }
}

void SK::VkRendererBackend::clearGPUAssets(State* vkRendererBackend, VkAssetRegistry* vkAssetRegistry)
{
    for (auto& mesh : vkAssetRegistry->meshes)
    {
        SK::VkRendererBackend::destroyBuffer(vkRendererBackend, mesh.meshBuffers.vertexBuffer);
        SK::VkRendererBackend::destroyBuffer(vkRendererBackend, mesh.meshBuffers.indexBuffer);
    }

    for (auto& texture : vkAssetRegistry->textures)
    {
        if (texture.ownsImage)
        {
            SK::VkRendererBackend::destroyImage(vkRendererBackend, texture.image);
        }
    }

    vkAssetRegistry->meshes.clear();
    vkAssetRegistry->textures.clear();
    vkAssetRegistry->meshIndexByName.clear();
    vkAssetRegistry->textureIndexByName.clear();
}
