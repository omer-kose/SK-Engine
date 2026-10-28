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

	void DescriptorHeap::init(State* vkRendererBackend, const DescriptorHeapDesc& desc)
	{
		assert(vkRendererBackend);
		assert(this);
		assert(!initialized);

		VkPhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeapProperties{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
			.pNext = nullptr
		};

		VkPhysicalDeviceProperties2 properties2{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &descriptorHeapProperties
		};

		maxResourceDescriptors = desc.maxResourceDescriptors;
		maxSamplerDescriptors = desc.maxSamplerDescriptors;

		vkGetPhysicalDeviceProperties2(vkRendererBackend->chosenGPU, &properties2);

		bufferDescriptorSize = descriptorHeapProperties.bufferDescriptorSize;
		imageDescriptorSize = descriptorHeapProperties.imageDescriptorSize;

		samplerDescriptorSize = descriptorHeapProperties.samplerDescriptorSize;

		const VkDeviceSize resourceDescriptorSize = std::max(bufferDescriptorSize, imageDescriptorSize);
		const VkDeviceSize resourceDescriptorAlignment = std::max(descriptorHeapProperties.bufferDescriptorAlignment, descriptorHeapProperties.imageDescriptorAlignment);
		resourceDescriptorStride = alignUp(resourceDescriptorSize, resourceDescriptorAlignment);
		samplerDescriptorStride = alignUp(samplerDescriptorSize, descriptorHeapProperties.samplerDescriptorAlignment);

		resourceHeapSize = alignUp(maxResourceDescriptors * resourceDescriptorStride + descriptorHeapProperties.minResourceHeapReservedRange, resourceDescriptorAlignment);
		samplerHeapSize = alignUp(maxSamplerDescriptors * samplerDescriptorStride + descriptorHeapProperties.minSamplerHeapReservedRange, descriptorHeapProperties.samplerDescriptorAlignment);

		resourceReservedRangeOffset = resourceHeapSize - descriptorHeapProperties.minResourceHeapReservedRange;
		resourceReservedRangeSize = descriptorHeapProperties.minResourceHeapReservedRange;
		samplerReservedRangeOffset = samplerHeapSize - descriptorHeapProperties.minSamplerHeapReservedRange;
		samplerReservedRangeSize = descriptorHeapProperties.minSamplerHeapReservedRange;

		resourceHeapBufferStaging = createBuffer(vkRendererBackend, resourceHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		resourceHeapStagingMappedData = static_cast<uint8_t*>(resourceHeapBufferStaging.allocInfo.pMappedData);
		resourceHeapBufferGPU = createBuffer(vkRendererBackend, resourceHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		samplerHeapBufferStaging = createBuffer(vkRendererBackend, samplerHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		samplerHeapStagingMappedData = static_cast<uint8_t*>(samplerHeapBufferStaging.allocInfo.pMappedData);
		samplerHeapBufferGPU = createBuffer(vkRendererBackend, samplerHeapSize, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

		s_bufferWrites = {};
		s_textureWrites = {};
		s_samplerInfos = {};
		s_samplerHostAddresses = {};

		initialized = true;
	}

	void DescriptorHeap::destroy(State* vkRendererBackend)
	{
		assert(vkRendererBackend);
		assert(this);

		if (!initialized)
		{
			return;
		}

		destroyBuffer(vkRendererBackend, resourceHeapBufferStaging);
		destroyBuffer(vkRendererBackend, resourceHeapBufferGPU);
		destroyBuffer(vkRendererBackend, samplerHeapBufferStaging);
		destroyBuffer(vkRendererBackend, samplerHeapBufferGPU);

		s_bufferWrites = {};
		s_textureWrites = {};
		s_samplerInfos = {};
		s_samplerHostAddresses = {};

		*this = {};
	}

	/*
		Staging buffers are in the CPU, so updating is done before anything is executed on the GPU. In other words, before staging buffers are uploaded they are guaranteed to be updated safely.
	*/
	void DescriptorHeap::update(State* vkRendererBackend)
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

	void DescriptorHeap::upload(State* vkRendererBackend, VkCommandBuffer cmd)
	{
		assert(vkRendererBackend);
		assert(this);
		assert(initialized);

		// Copy resource and sampler staging buffers to their corresponding GPU buffers.
		VkBufferCopy copy{ .srcOffset = 0, .dstOffset = 0 };
		copy.size = resourceHeapSize;
		vkCmdCopyBuffer(cmd, resourceHeapBufferStaging.buffer, resourceHeapBufferGPU.buffer, 1, &copy);
		copy.size = samplerHeapSize;
		vkCmdCopyBuffer(cmd, samplerHeapBufferStaging.buffer, samplerHeapBufferGPU.buffer, 1, &copy);

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

	void DescriptorHeap::bind(State* vkRendererBackend, VkCommandBuffer cmd)
	{
		assert(vkRendererBackend);
		assert(this);
		assert(initialized);

		VkBindHeapInfoEXT resourceHeapBindInfo{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.pNext = nullptr,
			.heapRange = VkDeviceAddressRangeEXT{
				.address = resourceHeapBufferGPU.address,
				.size = resourceHeapSize
			},
			.reservedRangeOffset = resourceReservedRangeOffset,
			.reservedRangeSize = resourceReservedRangeSize
		};

		vkCmdBindResourceHeapEXT(cmd, &resourceHeapBindInfo);

		VkBindHeapInfoEXT samplerHeapBindInfo{
			.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
			.pNext = nullptr,
			.heapRange = VkDeviceAddressRangeEXT{
				.address = samplerHeapBufferGPU.address,
				.size = samplerHeapSize
			},
			.reservedRangeOffset = samplerReservedRangeOffset,
			.reservedRangeSize = samplerReservedRangeSize
		};

		vkCmdBindSamplerHeapEXT(cmd, &samplerHeapBindInfo);
	}

	ResourceDescriptorHandle DescriptorHeap::allocateResourceDescriptor(ResourceDescriptorKind kind)
	{
		assert(this);
		assert(initialized);

		ResourceDescriptorHandle handle{};

		if (resourceFreeList.empty())
		{
			if (nextResourceDescriptor >= maxResourceDescriptors)
			{
				fmt::println("Resource Descriptor Heap is exhausted.");
				abort();
			}

			handle.index = nextResourceDescriptor;
			handle.kind = kind;

			++nextResourceDescriptor;
		}
		else
		{
			// Fetch the available slot and give that index to the handle
			handle.index = resourceFreeList.back();
			handle.kind = kind;

			resourceFreeList.pop_back();
		}

		return handle;
	}

	void DescriptorHeap::deleteDescriptor(ResourceDescriptorHandle& handle)
	{
		// Make the index of the handle available in the resource free list
		resourceFreeList.push_back(handle.index);

		// invalidate the handle
		handle.index = INVALID_RESOURCE_DESCRIPTOR_HANDLE;
	}

	SamplerDescriptorHandle DescriptorHeap::allocateSamplerDescriptor()
	{
		assert(this);
		assert(initialized);

		SamplerDescriptorHandle handle{};

		if (samplerFreeList.empty())
		{
			if (nextSamplerDescriptor >= maxSamplerDescriptors)
			{
				fmt::println("Sampler Descriptor Heap is exhausted.");
				abort();
			}

			handle.index = nextSamplerDescriptor;

			++nextSamplerDescriptor;
		}
		else
		{
			// Fetch the available slot and give that index to the handle
			handle.index = samplerFreeList.back();

			samplerFreeList.pop_back();
		}

		return handle;
	}

	void DescriptorHeap::deleteDescriptor(SamplerDescriptorHandle& handle)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		// Make the index of the handle available in the sampler free list
		samplerFreeList.push_back(handle.index);

		// invalidate the handle
		handle.index = INVALID_SAMPLER_DESCRIPTOR_HANDLE;
	}

	VkDeviceSize DescriptorHeap::getResourceDescriptorOffset(ResourceDescriptorHandle handle)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		assert(handle.index < maxResourceDescriptors);

		return handle.index * resourceDescriptorStride;
	}

	VkDeviceSize DescriptorHeap::getSamplerDescriptorOffset(SamplerDescriptorHandle handle)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		assert(handle.index < maxSamplerDescriptors);

		return handle.index * samplerDescriptorStride;
	}

	void DescriptorHeap::writeUniformBufferDescriptor(ResourceDescriptorHandle handle, const AllocatedBuffer& buffer, VkDeviceSize offset, VkDeviceSize range)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::UniformBuffer);

		VkDeviceAddressRangeEXT addressRange = VkDeviceAddressRangeEXT{ .address = buffer.address + offset, .size = range };

		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{
			.address = resourceHeapStagingMappedData + descriptorOffset,
			.size = bufferDescriptorSize
		};

		s_bufferWrites.push_back(BufferWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.addressRange = addressRange,
			.heapAddressRange = heapAddressRange
		});
	}

	void DescriptorHeap::writeStorageBufferDescriptor(ResourceDescriptorHandle handle, const AllocatedBuffer& buffer, VkDeviceSize offset, VkDeviceSize range)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::StorageBuffer);

		VkDeviceAddressRangeEXT addressRange = VkDeviceAddressRangeEXT{ .address = buffer.address + offset, .size = range };

		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{
			.address = resourceHeapStagingMappedData + descriptorOffset,
			.size = bufferDescriptorSize
		};

		s_bufferWrites.push_back(BufferWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.addressRange = addressRange,
			.heapAddressRange = heapAddressRange
		});
	}

	void DescriptorHeap::writeSampledImageDescriptor(ResourceDescriptorHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::SampledImage);
		assert(viewInfo.image != VK_NULL_HANDLE);

		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{ .address = resourceHeapStagingMappedData + descriptorOffset, .size = imageDescriptorSize };

		s_textureWrites.push_back(TextureWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.viewInfo = viewInfo,
			.layout = layout,
			.heapAddressRange = heapAddressRange
		});
	}

	void DescriptorHeap::writeStorageImageDescriptor(ResourceDescriptorHandle handle, const VkImageViewCreateInfo& viewInfo, VkImageLayout layout)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));
		assert(handle.kind == ResourceDescriptorKind::StorageImage);
		assert(viewInfo.image != VK_NULL_HANDLE);


		const VkDeviceSize descriptorOffset = getResourceDescriptorOffset(handle);

		// Where in the descriptor heap the descriptor info will be written onto.
		VkHostAddressRangeEXT heapAddressRange{ .address = resourceHeapStagingMappedData + descriptorOffset, .size = imageDescriptorSize };

		s_textureWrites.push_back(TextureWriteInfo{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.viewInfo = viewInfo,
			.layout = layout,
			.heapAddressRange = heapAddressRange
		});
	}

	void DescriptorHeap::writeSamplerDescriptor(SamplerDescriptorHandle handle, const VkSamplerCreateInfo& samplerInfo)
	{
		assert(this);
		assert(initialized);
		assert(isValid(handle));

		const VkDeviceSize samplerOffset = getSamplerDescriptorOffset(handle);

		VkHostAddressRangeEXT heapAddressRange{ .address = samplerHeapStagingMappedData + samplerOffset, .size = samplerDescriptorSize };

		// Store the necessary info to update later at once.
		s_samplerInfos.push_back(samplerInfo);
		s_samplerHostAddresses.push_back(heapAddressRange);
	}
}