#include "VkDescriptorHeap.h"

#include <RendererBackend/Vulkan/VkRendererBackend.h>

#include <algorithm>
#include <cassert>
#include <cstring>

static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
	if (alignment == 0)
	{
		return value;
	}

	return (value + alignment - 1) & ~(alignment - 1);
}

static void validateHeapCapacity(VkDeviceSize requestedSize, VkDeviceSize maxSize, const char* heapName)
{
	if (requestedSize > maxSize)
	{
		fmt::println(
			"{} heap size {} exceeds device limit {}.",
			heapName,
			requestedSize,
			maxSize);

		abort();
	}
}

void SK::VkRendererBackend::initDescriptorHeap(State* vkRendererBackend, DescriptorHeap* heap, const DescriptorHeapDesc& desc)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(!heap->initialized);

	VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
		.pNext = nullptr
	};

	VkPhysicalDeviceProperties2 properties2{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &descriptorHeapProperties
	};

	heap->maxResourceDescriptors = desc.maxResourceDescriptors;
	heap->maxSamplerDescriptors = desc.maxSamplerDescriptors;

	vkGetPhysicalDeviceProperties2(vkRendererBackend->chosenGPU, &properties2);

	heap->bufferDescriptorSize = descriptorHeapProperties.bufferDescriptorSize;
	heap->imageDescriptorSize = descriptorHeapProperties.imageDescriptorSize;

	heap->samplerDescriptorSize = descriptorHeapProperties.samplerDescriptorSize;

	const VkDeviceSize resourceDescriptorSize = std::max(heap->bufferDescriptorSize, heap->imageDescriptorSize);
	const VkDeviceSize resourceDescriptorAlignment = std::max(descriptorHeapProperties.bufferDescriptorAlignment, descriptorHeapProperties.imageDescriptorAlignment);
	heap->resourceDescriptorStride = alignUp(resourceDescriptorSize, resourceDescriptorAlignment);
	heap->samplerDescriptorStride = alignUp(heap->samplerDescriptorSize, descriptorHeapProperties.samplerDescriptorAlignment);

	heap->resourceHeapSize = alignUp(heap->maxResourceDescriptors * heap->resourceDescriptorStride + descriptorHeapProperties.minResourceHeapReservedRange, resourceDescriptorAlignment);
	heap->samplerHeapSize = alignUp(heap->maxSamplerDescriptors * heap->samplerDescriptorStride + descriptorHeapProperties.minSamplerHeapReservedRange, descriptorHeapProperties.samplerDescriptorAlignment);

	heap->resourceReservedRangeOffset = heap->resourceHeapSize - descriptorHeapProperties.minResourceHeapReservedRange;
	heap->resourceReservedRangeSize = descriptorHeapProperties.minResourceHeapReservedRange;
	heap->samplerReservedRangeOffset = heap->samplerHeapSize - descriptorHeapProperties.minSamplerHeapReservedRange;
	heap->samplerReservedRangeSize = descriptorHeapProperties.minSamplerHeapReservedRange;

	heap->resourceHeapBuffer = SK::VkRendererBackend::createBuffer(vkRendererBackend, heap->resourceHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	heap->resourceHeapMappedData = static_cast<uint8_t*>(heap->resourceHeapBuffer.allocInfo.pMappedData);

	heap->samplerHeapBuffer = SK::VkRendererBackend::createBuffer(vkRendererBackend, heap->samplerHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	heap->samplerHeapMappedData = static_cast<uint8_t*>(heap->samplerHeapBuffer.allocInfo.pMappedData);

	heap->initialized = true;
}

void SK::VkRendererBackend::destroyDescriptorHeap(State* vkRendererBackend, DescriptorHeap* heap)
{
	assert(vkRendererBackend);
	assert(heap);

	if (!heap->initialized)
	{
		return;
	}

	SK::VkRendererBackend::destroyBuffer(vkRendererBackend, heap->resourceHeapBuffer);
	SK::VkRendererBackend::destroyBuffer(vkRendererBackend, heap->samplerHeapBuffer);
	*heap = {};
}

void SK::VkRendererBackend::bindDescriptorHeap(State* vkRendererBackend, VkCommandBuffer cmd, const DescriptorHeap* heap)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(heap->initialized);

	VkBindHeapInfoEXT resourceHeapBindInfo{
		.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
		.pNext = nullptr,
		.heapRange = VkDeviceAddressRangeEXT{
			.address = heap->resourceHeapBuffer.address,
			.size = heap->resourceHeapSize
		},
		.reservedRangeOffset = heap->resourceReservedRangeOffset,
		.reservedRangeSize = heap->resourceReservedRangeSize
	};

	vkCmdBindResourceHeapEXT(cmd, &resourceHeapBindInfo);

	VkBindHeapInfoEXT samplerHeapBindInfo{
		.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
		.pNext = nullptr,
		.heapRange = VkDeviceAddressRangeEXT{
			.address = heap->samplerHeapBuffer.address,
			.size = heap->samplerHeapSize
		},
		.reservedRangeOffset = heap->samplerReservedRangeOffset,
		.reservedRangeSize = heap->samplerReservedRangeSize
	};

	vkCmdBindSamplerHeapEXT(cmd, &samplerHeapBindInfo);
}

SK::VkRendererBackend::ResourceDescriptorHandle SK::VkRendererBackend::allocateResourceDescriptor(DescriptorHeap* heap, ResourceDescriptorKind kind)
{
	assert(heap);
	assert(heap->initialized);

	ResourceDescriptorHandle handle{};

	if (heap->resourceFreeList.empty())
	{
		if (heap->nextResourceDescriptor >= heap->maxResourceDescriptors)
		{
			fmt::println("Resource Descriptor Heap is exhausted.");
			abort();
		}

		handle.index = heap->nextResourceDescriptor;
		handle.kind = kind;

		++heap->nextResourceDescriptor;
	}
	else
	{
		// Fetch the available slot and give that index to the handle
		fmt::println("Resource Descriptor index is available in the free list using the index: {}", heap->resourceFreeList.back());
		handle.index = heap->resourceFreeList.back();
		handle.kind = kind;

		heap->resourceFreeList.pop_back();
	}

	return handle;
}

void SK::VkRendererBackend::deleteDescriptor(DescriptorHeap* heap, ResourceDescriptorHandle& handle)
{
	// Make the index of the handle available in the resource free list
	heap->resourceFreeList.push_back(handle.index);

	// invalidate the handle
	handle.index = SK::VkRendererBackend::INVALID_RESOURCE_DESCRIPTOR_HANDLE;
}

SK::VkRendererBackend::SamplerDescriptorHandle SK::VkRendererBackend::allocateSamplerDescriptor(DescriptorHeap* heap)
{
	assert(heap);
	assert(heap->initialized);

	SamplerDescriptorHandle handle{};

	if (heap->samplerFreeList.empty())
	{
		if (heap->nextSamplerDescriptor >= heap->maxSamplerDescriptors)
		{
			fmt::println("Sampler Descriptor Heap is exhausted.");
			abort();
		}

		handle.index = heap->nextSamplerDescriptor;

		++heap->nextSamplerDescriptor;
	}
	else
	{
		// Fetch the available slot and give that index to the handle
		fmt::println("Sampler Descriptor index is available in the free list using the index: {}", heap->samplerFreeList.back());
		handle.index = heap->samplerFreeList.back();

		heap->samplerFreeList.pop_back();
	}
	
	return handle;
}

void SK::VkRendererBackend::deleteDescriptor(DescriptorHeap* heap, SamplerDescriptorHandle& handle)
{
	// Make the index of the handle available in the sampler free list
	heap->samplerFreeList.push_back(handle.index);

	// invalidate the handle
	handle.index = SK::VkRendererBackend::INVALID_SAMPLER_DESCRIPTOR_HANDLE;
}

VkDeviceSize SK::VkRendererBackend::getResourceDescriptorOffset(const DescriptorHeap* heap, ResourceDescriptorHandle handle)
{
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));
	assert(handle.index < heap->maxResourceDescriptors);

	return handle.index * heap->resourceDescriptorStride;
}

VkDeviceSize SK::VkRendererBackend::getSamplerDescriptorOffset(const DescriptorHeap* heap, SamplerDescriptorHandle handle)
{
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));
	assert(handle.index < heap->maxSamplerDescriptors);

	return handle.index * heap->samplerDescriptorStride;
}

void SK::VkRendererBackend::writeUniformBufferDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const AllocatedBuffer& buffer, VkDeviceSize offset, VkDeviceSize range)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));
	assert(handle.kind == ResourceDescriptorKind::UniformBuffer);

	VkDeviceAddressRangeEXT addressRange = VkDeviceAddressRangeEXT{ .address = buffer.address + offset,.size = range };

	VkResourceDescriptorDataEXT descriptorData{ .pAddressRange = &addressRange };

	VkResourceDescriptorInfoEXT descriptorInfo{
		.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
		.pNext = nullptr,
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.data = descriptorData
	};

	const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);
	
	// Where in the descriptor heap the descriptor info will be written onto.
	VkHostAddressRangeEXT heapAddressRange{
		.address = heap->resourceHeapMappedData + descriptorOffset,
		.size = heap->bufferDescriptorSize
	};

	VK_CHECK(vkWriteResourceDescriptorsEXT(vkRendererBackend->device, 1, &descriptorInfo, &heapAddressRange));
}

void SK::VkRendererBackend::writeStorageBufferDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const AllocatedBuffer& buffer, VkDeviceSize offset, VkDeviceSize range)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));
	assert(handle.kind == ResourceDescriptorKind::StorageBuffer);

	VkDeviceAddressRangeEXT addressRange = VkDeviceAddressRangeEXT{ .address = buffer.address + offset,.size = range };

	VkResourceDescriptorDataEXT descriptorData{ .pAddressRange = &addressRange };

	VkResourceDescriptorInfoEXT descriptorInfo{
		.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
		.pNext = nullptr,
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.data = descriptorData
	};

	const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

	// Where in the descriptor heap the descriptor info will be written onto.
	VkHostAddressRangeEXT heapAddressRange{
		.address = heap->resourceHeapMappedData + descriptorOffset,
		.size = heap->bufferDescriptorSize
	};

	VK_CHECK(vkWriteResourceDescriptorsEXT(vkRendererBackend->device, 1, &descriptorInfo, &heapAddressRange));
}

void SK::VkRendererBackend::writeSampledImageDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));
	assert(handle.kind == ResourceDescriptorKind::SampledImage);
	assert(viewInfo.image != VK_NULL_HANDLE);

	VkImageDescriptorInfoEXT imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
		.pNext = nullptr,
		.pView = &viewInfo,
		.layout = layout
	};

	VkResourceDescriptorDataEXT descriptorData{};
	descriptorData.pImage = &imageInfo;

	VkResourceDescriptorInfoEXT descriptorInfo{
		.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
		.pNext = nullptr,
		.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		.data = descriptorData
	};

	const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

	// Where in the descriptor heap the descriptor info will be written onto.
	VkHostAddressRangeEXT heapAddressRange{ .address = heap->resourceHeapMappedData + descriptorOffset, .size = heap->imageDescriptorSize };

	VK_CHECK(vkWriteResourceDescriptorsEXT(vkRendererBackend->device, 1, &descriptorInfo, &heapAddressRange));
}

void SK::VkRendererBackend::writeStorageImageDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));
	assert(handle.kind == ResourceDescriptorKind::StorageImage);
	assert(viewInfo.image != VK_NULL_HANDLE);

	VkImageDescriptorInfoEXT imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
		.pNext = nullptr,
		.pView = &viewInfo,
		.layout = layout
	};

	VkResourceDescriptorDataEXT descriptorData{};
	descriptorData.pImage = &imageInfo;

	VkResourceDescriptorInfoEXT descriptorInfo{
		.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
		.pNext = nullptr,
		.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.data = descriptorData
	};

	const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

	// Where in the descriptor heap the descriptor info will be written onto.
	VkHostAddressRangeEXT heapAddressRange{ .address = heap->resourceHeapMappedData + descriptorOffset, .size = heap->imageDescriptorSize };

	VK_CHECK(vkWriteResourceDescriptorsEXT(vkRendererBackend->device, 1, &descriptorInfo, &heapAddressRange));
}

void SK::VkRendererBackend::writeSamplerDescriptor(State* vkRendererBackend, DescriptorHeap* heap, SamplerDescriptorHandle handle, const VkSamplerCreateInfo& samplerInfo)
{
	assert(vkRendererBackend);
	assert(heap);
	assert(heap->initialized);
	assert(isValid(handle));

	const VkDeviceSize samplerOffset = getSamplerDescriptorOffset(heap, handle);

	VkHostAddressRangeEXT heapAddressRange{ .address = heap->samplerHeapMappedData + samplerOffset, .size = heap->samplerDescriptorSize };

	VK_CHECK(vkWriteSamplerDescriptorsEXT(vkRendererBackend->device, 1, &samplerInfo, &heapAddressRange));
}
