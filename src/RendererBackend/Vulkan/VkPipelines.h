#pragma once 
#include <RendererBackend/Vulkan/VkCommon.h>

namespace SK::VkRendererBackend
{
    struct ShaderResourceMapping
    {
        uint32_t binding;
        VkSpirvResourceTypeFlagsEXT type;
        uint32_t descriptorIndex;
    };
};

class PipelineBuilder
{
public:
    PipelineBuilder();
    void clear();
    VkPipeline buildPipeline(VkDevice device);

    void pushShaderStage(VkShaderModule shader, VkShaderStageFlagBits stage);
    void setInputTopology(VkPrimitiveTopology topology);
    void setPolygonMode(VkPolygonMode polygonMode);
    void setCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace);
    void setMultiSamplingNone();
    void disableBlending();
    void enableBlendingAdditive();
    void enableBlendingAlphaBlend();
    void setColorAttachmentFormat(VkFormat format);
    void setDepthFormat(VkFormat format);
    void disableDepthTest();
    void enableDepthTest(bool depthWriteEnable, VkCompareOp compareOp);

    void pushShaderResourceMapping(const SK::VkRendererBackend::ShaderResourceMapping& mapping, uint32_t heapArrayStride);
public:
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineColorBlendAttachmentState colorBlendAttachment;
    VkPipelineMultisampleStateCreateInfo multisampling;
    VkPipelineLayout pipelineLayout;
    VkPipelineDepthStencilStateCreateInfo depthStencil;
    VkPipelineRenderingCreateInfo renderInfo;
    VkFormat colorAttachmentformat;

    std::vector<VkDescriptorSetAndBindingMappingEXT> shaderResourceMappings;
};

namespace SK::VkUtil 
{
	bool loadShaderModule(VkDevice device, const char* filePath, VkShaderModule* outShaderModule);
};