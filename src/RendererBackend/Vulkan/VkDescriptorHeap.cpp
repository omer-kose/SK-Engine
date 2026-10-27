#include "VkDescriptorHeap.h"

#include <RendererBackend/Vulkan/VkRendererBackend.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace SK::VkRendererBackend
{
	// Internal types and data
	namespace
	{
		struct BufferWriteInfo
		{
			VkDescriptorType type; // uniform or storage
			VkDeviceAddressRangeEXT addressRange;
			VkHostAddressRangeEXT heapAddressRange;
		};

		struct TextureWriteInfo
		{
			VkDescriptorType type; // sampled or storage
			VkImageViewCreateInfo viewInfo;
			VkImageLayout layout;
			VkHostAddressRangeEXT heapAddressRange;
		};

		// Deferred resource and sampler writes to update the descriptors at once.
		std::vector<BufferWriteInfo> s_bufferWrites;
		std::vector<TextureWriteInfo> s_textureWrites;
		std::vector<VkSamplerCreateInfo> s_samplerInfos;
		std::vector<VkHostAddressRangeEXT> s_samplerHostAddresses;
	}

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

	void initDescriptorHeap(State* vkRendererBackend, DescriptorHeap* heap, const DescriptorHeapDesc& desc)
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

		heap->resourceHeapBufferStaging = createBuffer(vkRendererBackend, heap->resourceHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		heap->resourceHeapStagingMappedData = static_cast<uint8_t*>(heap->resourceHeapBufferStaging.allocInfo.pMappedData);
		heap->resourceHeapBufferGPU = createBuffer(vkRendererBackend, heap->resourceHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		heap->samplerHeapBufferStaging = createBuffer(vkRendererBackend, heap->samplerHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		heap->samplerHeapStagingMappedData = static_cast<uint8_t*>(heap->samplerHeapBufferStaging.allocInfo.pMappedData);
		heap->samplerHeapBufferGPU = createBuffer(vkRendererBackend, heap->samplerHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		s_bufferWrites = {};
		s_textureWrites = {};
		s_samplerInfos = {};
		s_samplerHostAddresses = {};

		heap->initialized = true;
	}

	void destroyDescriptorHeap(State* vkRendererBackend, DescriptorHeap* heap)
	{
		assert(vkRendererBackend);
		assert(heap);

		if (!heap->initialized)
		{
			return;
		}

		destroyBuffer(vkRendererBackend, heap->resourceHeapBufferStaging);
		destroyBuffer(vkRendererBackend, heap->resourceHeapBufferGPU);
		destroyBuffer(vkRendererBackend, heap->samplerHeapBufferStaging);
		destroyBuffer(vkRendererBackend, heap->samplerHeapBufferGPU);

		s_bufferWrites = {};
		s_textureWrites = {};
		s_samplerInfos = {};
		s_samplerHostAddresses = {};

		*heap = {};
	}

	/*
		Staging buffers are in the CPU, so updating is done before anything is executed on the GPU. In other words, before staging buffers are uploaded they are guaranteed to be updated safely.
	*/
	void updateDescriptorHeap(State* vkRendererBackend, DescriptorHeap* heap)
	{
		// Update the descriptor heap staging buffers with the requested writes.
		size_t resourceDescriptorCount = s_bufferWrites.size() + s_textureWrites.size();
		if (resourceDescriptorCount > 0)
		{
			std::vector<VkImageDescriptorInfoEXT> imageInfos;
			imageInfos.reserve(resourceDescriptorCount);
			std::vector<VkResourceDescriptorInfoEXT> resourceDescriptorInfos;
			resourceDescriptorInfos.reserve(resourceDescriptorCount);
			std::vector<VkHostAddressRangeEXT> resourceHostAddresses;
			resourceHostAddresses.reserve(resourceDescriptorCount);

			for (const BufferWriteInfo& info : s_bufferWrites)
			{
				VkResourceDescriptorDataEXT descriptorData{ .pAddressRange = &info.addressRange };

				VkResourceDescriptorInfoEXT descriptorInfo{
					.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
					.pNext = nullptr,
					.type = info.type,
					.data = descriptorData
				};

				resourceDescriptorInfos.push_back(descriptorInfo);
				resourceHostAddresses.push_back(info.heapAddressRange);
			}

			for (const TextureWriteInfo& info : s_textureWrites)
			{
				imageInfos.push_back(VkImageDescriptorInfoEXT{
					.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
					.pNext = nullptr,
					.pView = &info.viewInfo,
					.layout = info.layout
				});

				VkResourceDescriptorDataEXT descriptorData{};
				descriptorData.pImage = &imageInfos.back();

				VkResourceDescriptorInfoEXT descriptorInfo{
					.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
					.pNext = nullptr,
					.type = info.type,
					.data = descriptorData
				};

				resourceDescriptorInfos.push_back(descriptorInfo);
				resourceHostAddresses.push_back(info.heapAddressRange);
			}

			VK_CHECK(vkWriteResourceDescriptorsEXT(vkRendererBackend->device, resourceDescriptorCount, resourceDescriptorInfos.data(), resourceHostAddresses.data()));

			// Flush the requests
			s_bufferWrites.clear();
			s_textureWrites.clear();
		}

		if (s_samplerInfos.size() > 0)
		{
			VK_CHECK(vkWriteSamplerDescriptorsEXT(vkRendererBackend->device, s_samplerInfos.size(), s_samplerInfos.data(), s_samplerHostAddresses.data()));
			s_samplerInfos.clear();
			s_samplerHostAddresses.clear();
		}
	}

	void uploadDescriptorHeap(State* vkRendererBackend, VkCommandBuffer cmd, DescriptorHeap* heap)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);

		// Copy resource and sampler staging buffers to their corresponding GPU buffers.
		VkBufferCopy copy{ .srcOffset = 0, .dstOffset = 0 };
		copy.size = heap->resourceHeapSize;
		vkCmdCopyBuffer(cmd, heap->resourceHeapBufferStaging.buffer, heap->resourceHeapBufferGPU.buffer, 1, &copy);
		copy.size = heap->samplerHeapSize;
		vkCmdCopyBuffer(cmd, heap->samplerHeapBufferStaging.buffer, heap->samplerHeapBufferGPU.buffer, 1, &copy);

		// Issue a barrier at the beginning of the frame to synchronize.
		VkMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask =
				VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.dstAccessMask =
				VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT |
				VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT,
		};

		VkDependencyInfo dependency{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &barrier,
		};

		vkCmdPipelineBarrier2(cmd, &dependency);
	}

	void bindDescriptorHeap(State* vkRendererBackend, VkCommandBuffer cmd, const DescriptorHeap* heap)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);

		VkBindHeapInfoEXT resourceHeapBindInfo{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.pNext = nullptr,
			.heapRange = VkDeviceAddressRangeEXT{
				.address = heap->resourceHeapBufferGPU.address,
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
				.address = heap->samplerHeapBufferGPU.address,
				.size = heap->samplerHeapSize
			},
			.reservedRangeOffset = heap->samplerReservedRangeOffset,
			.reservedRangeSize = heap->samplerReservedRangeSize
		};

		vkCmdBindSamplerHeapEXT(cmd, &samplerHeapBindInfo);
	}

	ResourceDescriptorHandle allocateResourceDescriptor(DescriptorHeap* heap, ResourceDescriptorKind kind)
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
			handle.index = heap->resourceFreeList.back();
			handle.kind = kind;

			heap->resourceFreeList.pop_back();
		}

		return handle;
	}

	void deleteDescriptor(DescriptorHeap* heap, ResourceDescriptorHandle& handle)
	{
		// Make the index of the handle available in the resource free list
		heap->resourceFreeList.push_back(handle.index);

		// invalidate the handle
		handle.index = INVALID_RESOURCE_DESCRIPTOR_HANDLE;
	}

	SamplerDescriptorHandle allocateSamplerDescriptor(DescriptorHeap* heap)
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
			handle.index = heap->samplerFreeList.back();

			heap->samplerFreeList.pop_back();
		}

		return handle;
	}

	void deleteDescriptor(DescriptorHeap* heap, SamplerDescriptorHandle& handle)
	{
		// Make the index of the handle available in the sampler free list
		heap->samplerFreeList.push_back(handle.index);

		// invalidate the handle
		handle.index = INVALID_SAMPLER_DESCRIPTOR_HANDLE;
	}

	VkDeviceSize getResourceDescriptorOffset(const DescriptorHeap* heap, ResourceDescriptorHandle handle)
	{
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));
		assert(handle.index < heap->maxResourceDescriptors);

		return handle.index * heap->resourceDescriptorStride;
	}

	VkDeviceSize getSamplerDescriptorOffset(const DescriptorHeap* heap, SamplerDescriptorHandle handle)
	{
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));
		assert(handle.index < heap->maxSamplerDescriptors);

		return handle.index * heap->samplerDescriptorStride;
	}

	void writeUniformBufferDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const AllocatedBuffer& buffer, VkDeviceSize offset, VkDeviceSize range)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::UniformBuffer);

		VkDeviceAddressRangeEXT addressRange = VkDeviceAddressRangeEXT{ .address = buffer.address + offset, .size = range };

		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{
			.address = heap->resourceHeapStagingMappedData + descriptorOffset,
			.size = heap->bufferDescriptorSize
		};

		s_bufferWrites.push_back(BufferWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.addressRange = addressRange,
			.heapAddressRange = heapAddressRange
		});
	}

	void writeStorageBufferDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const AllocatedBuffer& buffer, VkDeviceSize offset, VkDeviceSize range)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::StorageBuffer);

		VkDeviceAddressRangeEXT addressRange = VkDeviceAddressRangeEXT{ .address = buffer.address + offset, .size = range };

		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{
			.address = heap->resourceHeapStagingMappedData + descriptorOffset,
			.size = heap->bufferDescriptorSize
		};

		s_bufferWrites.push_back(BufferWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.addressRange = addressRange,
			.heapAddressRange = heapAddressRange
		});
	}

	void writeSampledImageDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::SampledImage);
		assert(viewInfo.image != VK_NULL_HANDLE);

		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{ .address = heap->resourceHeapStagingMappedData + descriptorOffset, .size = heap->imageDescriptorSize };

		s_textureWrites.push_back(TextureWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.viewInfo = viewInfo,
			.layout = layout,
			.heapAddressRange = heapAddressRange
		});
	}

	void writeStorageImageDescriptor(State* vkRendererBackend, DescriptorHeap* heap, ResourceDescriptorHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::StorageImage);
		assert(viewInfo.image != VK_NULL_HANDLE);


		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(heap, handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{ .address = heap->resourceHeapStagingMappedData + descriptorOffset, .size = heap->imageDescriptorSize };

		s_textureWrites.push_back(TextureWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.viewInfo = viewInfo,
			.layout = layout,
			.heapAddressRange = heapAddressRange
		});
	}

	void writeSamplerDescriptor(State* vkRendererBackend, DescriptorHeap* heap, SamplerDescriptorHandle handle, const VkSamplerCreateInfo& samplerInfo)
	{
		assert(vkRendererBackend);
		assert(heap);
		assert(heap->initialized);
		assert(isValid(handle));

		const VkDeviceSize samplerOffset = getSamplerDescriptorOffset(heap, handle);

		VkHostAddressRangeEXT heapAddressRange{ .address = heap->samplerHeapStagingMappedData + samplerOffset, .size = heap->samplerDescriptorSize };

		// Store the necessary info to update later at once.
		s_samplerInfos.push_back(samplerInfo);
		s_samplerHostAddresses.push_back(heapAddressRange);
	}
}