#include <RendererBackend/Vulkan/VkImages.h>

#include <RendererBackend/Vulkan/VkInitializers.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/*
    Image layout transitions in Vulkan done via a pipeline barrier. Barriers are quite precise. This function is overly general and blocks the whole pipeline and thus should never be used in the middle of rendering logic.

    It should only be used before and after rendering operations as there will be no operations to block during the execution of the command buffer. 
    I only use this to in the beginning and end of the frame so it doesn't create any bottlenecks but any other transitions needed during rendering should be done with proper and precise pipeline barriers.
*/
void SK::VkUtil::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout)
{
	VkImageMemoryBarrier2 imageBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = nullptr};
	
	imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

	imageBarrier.oldLayout = currentLayout;
	imageBarrier.newLayout = newLayout;

	VkImageAspectFlags aspectMask = (newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	imageBarrier.subresourceRange = SK::VkInit::image_subresource_range(aspectMask);
	imageBarrier.image = image;

	VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = nullptr};
	depInfo.imageMemoryBarrierCount = 1;
	depInfo.pImageMemoryBarriers = &imageBarrier;

	vkCmdPipelineBarrier2(cmd, &depInfo);
}

void SK::VkUtil::blitImage(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D destSize)
{
	VkImageBlit2 blitRegion = {.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr};

	blitRegion.srcOffsets[1].x = srcSize.width;
	blitRegion.srcOffsets[1].y = srcSize.height;
	blitRegion.srcOffsets[1].z = 1;

	blitRegion.dstOffsets[1].x = destSize.width;
	blitRegion.dstOffsets[1].y = destSize.height;
	blitRegion.dstOffsets[1].z = 1;

	blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.srcSubresource.baseArrayLayer = 0;
	blitRegion.srcSubresource.layerCount = 1;
	blitRegion.srcSubresource.mipLevel = 0;

	blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blitRegion.dstSubresource.baseArrayLayer = 0;
	blitRegion.dstSubresource.layerCount = 1;
	blitRegion.dstSubresource.mipLevel = 0;

	VkBlitImageInfo2 blitInfo = {.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr};
	blitInfo.dstImage = destination;
	blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	blitInfo.srcImage = source;
	blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	blitInfo.filter = VK_FILTER_LINEAR;
	blitInfo.regionCount = 1;
	blitInfo.pRegions = &blitRegion;

	vkCmdBlitImage2(cmd, &blitInfo);
}

void SK::VkUtil::generateMipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize)
{
    int mipLevels = int(std::floor(std::log2(std::max(imageSize.width, imageSize.height)))) + 1;
    for(int mip = 0; mip < mipLevels; ++mip) 
    {
        VkExtent2D halfSize = imageSize;
        halfSize.width /= 2;
        halfSize.height /= 2;

        VkImageMemoryBarrier2 imageBarrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .pNext = nullptr };

        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        imageBarrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBarrier.subresourceRange = SK::VkInit::image_subresource_range(aspectMask);
        imageBarrier.subresourceRange.levelCount = 1;
        imageBarrier.subresourceRange.baseMipLevel = mip;
        imageBarrier.image = image;

        VkDependencyInfo depInfo{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .pNext = nullptr };
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &imageBarrier;

        vkCmdPipelineBarrier2(cmd, &depInfo);

        if(mip < mipLevels - 1) 
        {
            VkImageBlit2 blitRegion{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .pNext = nullptr };

            blitRegion.srcOffsets[1].x = imageSize.width;
            blitRegion.srcOffsets[1].y = imageSize.height;
            blitRegion.srcOffsets[1].z = 1;

            blitRegion.dstOffsets[1].x = halfSize.width;
            blitRegion.dstOffsets[1].y = halfSize.height;
            blitRegion.dstOffsets[1].z = 1;

            blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blitRegion.srcSubresource.baseArrayLayer = 0;
            blitRegion.srcSubresource.layerCount = 1;
            blitRegion.srcSubresource.mipLevel = mip;

            blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blitRegion.dstSubresource.baseArrayLayer = 0;
            blitRegion.dstSubresource.layerCount = 1;
            blitRegion.dstSubresource.mipLevel = mip + 1;

            VkBlitImageInfo2 blitInfo{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .pNext = nullptr };
            blitInfo.dstImage = image;
            blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitInfo.srcImage = image;
            blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitInfo.filter = VK_FILTER_LINEAR;
            blitInfo.regionCount = 1;
            blitInfo.pRegions = &blitRegion;

            vkCmdBlitImage2(cmd, &blitInfo);

            imageSize = halfSize;
        }
    }

    // transition all mip levels into the final read_only layout
    transitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
