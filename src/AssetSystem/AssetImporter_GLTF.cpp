#include "AssetImporter_GLTF.h"

#include <fmt/core.h>
#include <fmt/format.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include "AssetRegistry.h"

#include "stb_image.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <glm/gtx/quaternion.hpp>

#include <Renderer/GlobalGPUTypes.h>

static SK::Asset::TextureFilter extractTextureFilter(fastgltf::Filter filter)
{
    switch (filter)
    {
        // nearest samplers
    case fastgltf::Filter::Nearest:
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::NearestMipMapLinear:
        return SK::Asset::TextureFilter::NEAREST;

        //linear samplers
    case fastgltf::Filter::Linear:
    case fastgltf::Filter::LinearMipMapNearest:
    case fastgltf::Filter::LinearMipMapLinear:
    default:
        return SK::Asset::TextureFilter::LINEAR;
    }
}

static SK::Asset::TextureMipmapMode extractTextureMipmapMode(fastgltf::Filter filter)
{
    switch (filter)
    {
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::LinearMipMapNearest:
        return SK::Asset::TextureMipmapMode::NEAREST;

    case fastgltf::Filter::NearestMipMapLinear:
    case fastgltf::Filter::LinearMipMapLinear:
    default:
        return SK::Asset::TextureMipmapMode::LINEAR;
    }
}

static bool hasMipmapFilter(fastgltf::Filter filter)
{
    switch (filter)
    {
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::LinearMipMapNearest:
    case fastgltf::Filter::NearestMipMapLinear:
    case fastgltf::Filter::LinearMipMapLinear:
        return true;
    default:
        return false;
    }
}

static std::optional<SK::Asset::RawImage> loadRawImageFromGLTF(const std::filesystem::path& basePath, fastgltf::Asset& asset, fastgltf::Image& image)
{
    int width = 0, height = 0, channels = 0;

    SK::Asset::RawImage out{};

    auto tryAssign = [&](unsigned char* data, int w, int h, int c)
        {
            if (!data)
            {
                return false;
            }

            out.width = static_cast<uint32_t>(w);
            out.height = static_cast<uint32_t>(h);
            out.channels = 4; // force RGBA
            out.data.assign(data, data + (w * h * 4));
            stbi_image_free(data);
            return true;
        };

    bool loaded = false;

    // NOTE: Buffer/image data is now stored as std::byte (not std::uint8_t), and when
    // Options::LoadExternalBuffers / LoadGLBBuffers is used the data typically arrives
    // as fastgltf::sources::Array rather than fastgltf::sources::Vector, so both are
    // handled here defensively.
    std::visit(fastgltf::visitor
        {
            [](auto&) {},
            [&](fastgltf::sources::URI& filePath)
            {
                assert(filePath.fileByteOffset == 0);
                assert(filePath.uri.isLocalPath());
                std::filesystem::path fullPath = basePath / filePath.uri.fspath();
                unsigned char* data = stbi_load(fullPath.string().c_str(), &width, &height, &channels, 4);
                loaded = tryAssign(data, width, height, channels);
            },
            [&](fastgltf::sources::Array& array)
            {
                unsigned char* data = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(array.bytes.data()),
                    static_cast<int>(array.bytes.size()), &width, &height, &channels, 4);
                loaded = tryAssign(data, width, height, channels);
            },
            [&](fastgltf::sources::Vector& vec)
            {
                unsigned char* data = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(vec.bytes.data()),
                    static_cast<int>(vec.bytes.size()), &width, &height, &channels, 4);
                loaded = tryAssign(data, width, height, channels);
            },
            [&](fastgltf::sources::BufferView& view)
            {
                auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                auto& buffer = asset.buffers[bufferView.bufferIndex];

                std::visit(fastgltf::visitor
                {
                    [](auto&) {},
                    [&](fastgltf::sources::Array& array)
                    {
                        unsigned char* data = stbi_load_from_memory(
                            reinterpret_cast<const unsigned char*>(array.bytes.data()) + bufferView.byteOffset,
                            static_cast<int>(bufferView.byteLength), &width, &height, &channels, 4);
                        loaded = tryAssign(data, width, height, channels);
                    },
                    [&](fastgltf::sources::Vector& vec)
                    {
                        unsigned char* data = stbi_load_from_memory(
                            reinterpret_cast<const unsigned char*>(vec.bytes.data()) + bufferView.byteOffset,
                            static_cast<int>(bufferView.byteLength), &width, &height, &channels, 4);
                        loaded = tryAssign(data, width, height, channels);
                    }
                }, buffer.data);
            }
        }, image.data);

    if (!loaded)
    {
        return {};
    }

    return out;
}

bool SK::Asset::importGLTF(std::string_view filePath, ImportedAsset* outAsset)
{
    if (!outAsset)
    {
        return false;
    }

    outAsset->meshes.clear();
    outAsset->textures.clear();
    outAsset->materials.clear();
    outAsset->gltfScene.reset();

    fmt::println("Loading GLTF: {}", filePath);

    std::filesystem::path gltfPath = filePath;
    std::string_view gltfFileName = filePath.substr(filePath.find_last_of("/") + 1);
    gltfFileName = gltfFileName.substr(0, gltfFileName.find_last_of("."));

    fastgltf::Parser parser{};
    constexpr auto gltfOptions = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble | fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;

    // GltfDataBuffer::loadFromFile() no longer exists; loading now goes through the
    // FromPath() factory, which returns an Expected<GltfDataBuffer>.
    auto gltfFile = fastgltf::GltfDataBuffer::FromPath(gltfPath);
    if (!gltfFile)
    {
        fmt::println("Failed to open GLTF file: {} (error {})", filePath, static_cast<uint64_t>(gltfFile.error()));
        return false;
    }

    // loadGLTF/loadBinaryGLTF + manual determineGltfFileType() have been replaced by a
    // single loadGltf() call that auto-detects glTF vs GLB. It takes a GltfDataGetter&
    // (not a pointer).
    auto load = parser.loadGltf(gltfFile.get(), gltfPath.parent_path(), gltfOptions);
    if (!load)
    {
        fmt::println("Failed to load GLTF: {} (error {})", filePath, static_cast<uint64_t>(load.error()));
        return false;
    }

    fastgltf::Asset asset = std::move(load.get());

    // Textures
    outAsset->textures.reserve(asset.textures.size());

    for (size_t i = 0; i < asset.textures.size(); ++i)
    {
        const fastgltf::Texture& gltfTexture = asset.textures[i];

        RawTexture texture{};
        texture.name = gltfTexture.name.empty() ? fmt::format("gltf_{}_texture_{}", gltfFileName, i) : gltfTexture.name.c_str();

        // Mipmap hint
        if (gltfTexture.samplerIndex.has_value())
        {
            const fastgltf::Sampler& sampler = asset.samplers[gltfTexture.samplerIndex.value()];
            if (sampler.minFilter.has_value())
            {
                texture.description.mipmapped = hasMipmapFilter(sampler.minFilter.value());
            }
        }

        // Sampler info (if not provided default values will be used for sampler creation)
        if (gltfTexture.samplerIndex.has_value())
        {
            fastgltf::Sampler& gltfSampler = asset.samplers[gltfTexture.samplerIndex.value()];
            texture.description.minFilter = extractTextureFilter(gltfSampler.minFilter.value_or(fastgltf::Filter::Nearest));
            texture.description.magFilter = extractTextureFilter(gltfSampler.magFilter.value_or(fastgltf::Filter::Nearest));

            texture.description.mipmapMode = extractTextureMipmapMode(gltfSampler.minFilter.value_or(fastgltf::Filter::Nearest));
        }

        if (gltfTexture.imageIndex.has_value())
        {
            fastgltf::Image& gltfImage = asset.images[gltfTexture.imageIndex.value()];
            auto img = loadRawImageFromGLTF(gltfPath.parent_path(), asset, gltfImage);
            if (img.has_value())
            {
                texture.image = std::move(img.value());
            }
            else
            {
                fmt::println("Failed to load image for texture: {}", texture.name);
            }
        }
        else
        {
            fmt::println("Texture {} has no image", texture.name);
        }

        outAsset->textures.push_back(std::move(texture));
    }

    // Materials
    outAsset->materials.reserve(asset.materials.size());
    for (size_t i = 0; i < asset.materials.size(); ++i)
    {
        const fastgltf::Material& gltfMat = asset.materials[i];

        SK::Material::Instance mat{};
        mat.alphaMode = SK::Material::AlphaMode::Opaque;
        if (gltfMat.alphaMode == fastgltf::AlphaMode::Blend)
        {
            mat.alphaMode = SK::Material::AlphaMode::Transparent;
        }

        SK::Material::PBRData pbrData{};
        pbrData.baseColorFactor[0] = gltfMat.pbrData.baseColorFactor[0];
        pbrData.baseColorFactor[1] = gltfMat.pbrData.baseColorFactor[1];
        pbrData.baseColorFactor[2] = gltfMat.pbrData.baseColorFactor[2];
        pbrData.baseColorFactor[3] = gltfMat.pbrData.baseColorFactor[3];
        pbrData.metallicFactor = gltfMat.pbrData.metallicFactor;
        pbrData.roughnessFactor = gltfMat.pbrData.roughnessFactor;

        if (gltfMat.pbrData.baseColorTexture.has_value())
        {
            pbrData.baseColorTexture = static_cast<uint32_t>(gltfMat.pbrData.baseColorTexture->textureIndex);
        }

        if (gltfMat.pbrData.metallicRoughnessTexture.has_value())
        {
            pbrData.metallicRoughnessTexture = static_cast<uint32_t>(gltfMat.pbrData.metallicRoughnessTexture->textureIndex);
        }

        if (gltfMat.normalTexture.has_value())
        {
            pbrData.normalTexture = static_cast<uint32_t>(gltfMat.normalTexture->textureIndex);
        }

        mat.materialData = pbrData;

        outAsset->materials.push_back(std::move(mat));
    }

    // Meshes
    std::vector<uint32_t> indices;
    std::vector<SK::Renderer::Vertex> vertices;

    outAsset->meshes.reserve(asset.meshes.size());

    for (size_t i = 0; i < asset.meshes.size(); ++i)
    {
        fastgltf::Mesh& gltfMesh = asset.meshes[i];

        RawMesh mesh{};
        mesh.name = gltfMesh.name.empty() ? fmt::format("gltf_{}_mesh_{}", gltfFileName, i) : gltfMesh.name.c_str();

        indices.clear();
        vertices.clear();
        mesh.subMeshes.clear();

        for (auto& primitive : gltfMesh.primitives)
        {
            SubMesh subMesh{};
            subMesh.startIndex = static_cast<uint32_t>(indices.size());

            const size_t initialVertex = vertices.size();

            // Indices
            if (primitive.indicesAccessor.has_value())
            {
                fastgltf::Accessor& indexAccessor = asset.accessors[primitive.indicesAccessor.value()];
                subMesh.indexCount = static_cast<uint32_t>(indexAccessor.count);

                indices.reserve(indices.size() + indexAccessor.count);

                fastgltf::iterateAccessor<uint32_t>(asset, indexAccessor, [&](uint32_t idx)
                    {
                        indices.push_back(idx + static_cast<uint32_t>(initialVertex));
                    });
            }
            else
            {
                fmt::println("Index accessor missing in mesh: {}", mesh.name);
                return false;
            }

            // Positions
            auto postAttr = primitive.findAttribute("POSITION");
            if (postAttr != primitive.attributes.end())
            {
                fastgltf::Accessor& posAccessor = asset.accessors[postAttr->accessorIndex];

                const size_t vertexOffset = vertices.size();
                vertices.resize(vertices.size() + posAccessor.count);

                fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAccessor,
                    [&](glm::vec3 v, size_t index)
                    {
                        SK::Renderer::Vertex& out = vertices[vertexOffset + index];
                        out.position = v;
                        out.normal = { 0, 0, 0 };
                        out.tangent = glm::vec4{ 0.0f };
                        out.uv_x = 0;
                        out.uv_y = 0;
                    });
            }
            else
            {
                fmt::println("Position attribute missing in mesh: {}", mesh.name);
                return false;
            }

            // Normals
            if (auto attr = primitive.findAttribute("NORMAL"); attr != primitive.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, asset.accessors[attr->accessorIndex],
                    [&](glm::vec3 v, size_t index)
                    {
                        vertices[initialVertex + index].normal = v;
                    });
            }

            // Tangents
            if (auto attr = primitive.findAttribute("TANGENT"); attr != primitive.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, asset.accessors[attr->accessorIndex],
                    [&](glm::vec4 v, size_t index)
                    {
                        vertices[initialVertex + index].tangent = v;
                    });
            }

            // UVs
            if (auto attr = primitive.findAttribute("TEXCOORD_0"); attr != primitive.attributes.end())
            {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, asset.accessors[attr->accessorIndex],
                    [&](glm::vec2 v, size_t index)
                    {
                        vertices[initialVertex + index].uv_x = v.x;
                        vertices[initialVertex + index].uv_y = v.y;
                    });
            }


            // Bounds
            glm::vec3 minPos = vertices[initialVertex].position;
            glm::vec3 maxPos = minPos;

            for (size_t v = initialVertex; v < vertices.size(); ++v)
            {
                minPos = glm::min(minPos, vertices[v].position);
                maxPos = glm::max(maxPos, vertices[v].position);
            }

            subMesh.bounds.origin = (minPos + maxPos) * 0.5f;
            subMesh.bounds.extents = (maxPos - minPos) * 0.5f;
            subMesh.bounds.sphereRadius = glm::length(subMesh.bounds.extents);

            // Material
            if (primitive.materialIndex.has_value())
            {
                subMesh.materialIndex = static_cast<uint32_t>(primitive.materialIndex.value());
            }
            else
            {
                subMesh.materialIndex = SK::Material::INVALID_MATERIAL;
                size_t idx = &primitive - gltfMesh.primitives.data();
                fmt::println("Mesh {} Submesh {} has no material assigned", mesh.name, idx);
            }

            mesh.subMeshes.push_back(subMesh);
        }

        mesh.vertices = std::move(vertices);
        mesh.indices = std::move(indices);

        outAsset->meshes.push_back(std::move(mesh));
    }

    // GLTF Scene
    if (!asset.nodes.empty())
    {
        outAsset->gltfScene.emplace();
        GLTFScene& scene = outAsset->gltfScene.value();
        scene.name = gltfFileName;

        scene.nodes.resize(asset.nodes.size());

        // Fill nodes + transforms
        for (size_t i = 0; i < asset.nodes.size(); ++i)
        {
            fastgltf::Node& n = asset.nodes[i];
            GLTFSceneNode& node = scene.nodes[i];

            node.meshIndex = n.meshIndex.has_value() ? static_cast<int>(n.meshIndex.value()) : -1;

            // Node::transform is now std::variant<TRS, fastgltf::math::fmat4x4> (fastgltf's
            // own math types), instead of the old fastgltf::Node::TransformMatrix/TRS pair.
            // A generic lambda + if constexpr avoids having to name the (possibly nested)
            // TRS type explicitly.
            std::visit([&](auto&& transform)
                {
                    using T = std::decay_t<decltype(transform)>;

                    if constexpr (std::is_same_v<T, fastgltf::math::fmat4x4>)
                    {
                        static_assert(sizeof(T) == sizeof(glm::mat4), "fastgltf::math::fmat4x4 layout mismatch with glm::mat4");
                        memcpy(&node.localTransform, &transform, sizeof(glm::mat4));
                    }
                    else
                    {
                        glm::vec3 t(transform.translation[0], transform.translation[1], transform.translation[2]);
                        glm::quat r(transform.rotation[3], transform.rotation[0], transform.rotation[1], transform.rotation[2]);
                        glm::vec3 s(transform.scale[0], transform.scale[1], transform.scale[2]);

                        glm::mat4 tm = glm::translate(glm::mat4(1.0f), t);
                        glm::mat4 rm = glm::toMat4(r);
                        glm::mat4 sm = glm::scale(glm::mat4(1.0f), s);

                        node.localTransform = tm * rm * sm;
                    }
                }, n.transform);

            node.children.reserve(n.children.size());
            for (auto c : n.children)
            {
                node.children.push_back(static_cast<int>(c));
                scene.nodes[c].parent = static_cast<int>(i);
            }
        }

        // Roots
        for (size_t i = 0; i < scene.nodes.size(); ++i)
        {
            if (scene.nodes[i].parent == -1)
            {
                scene.rootNodes.push_back(static_cast<int>(i));
            }
        }
    }

    return true;
}