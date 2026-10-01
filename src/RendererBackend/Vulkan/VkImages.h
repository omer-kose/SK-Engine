#pragma once 

#include "volk.h"

namespace SK::VkUtil 
{
	void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);

	void blitImage(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D destSize);

	void generateMipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);
};