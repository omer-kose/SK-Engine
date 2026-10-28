#pragma once

#include <RendererBackend/Vulkan/VkTypes.h>

#include <cstdint>
#include <limits>

/*
	Similar to DX12, Vulkan has 2 kind of descriptor heaps (resource and sampler). Each of them is called a heap. This utility struct actually contains both resource and sampler heaps in it. 
	This makes handling stuff easier as we will have a single descriptor heap for the whole program.

	Resource heap is unified. In other words, any type of descriptor can be stored at any slot. This requires setting the descriptor stride to the maximum of available descriptor strides. So, it wastes
	memory for smaller descriptor blobs but at the same time it makes allocating slots as needed instead of fixing them to certain capacities. This follows DX12's Heap design.

	Another way could be dividing the heap into certain ranges per resource descriptor kind. In that way, same type of descriptor blobs would be contiguous. However, unusued slots in ranges wouldn't be used and wasted.
	In an end product, those capacities could be set accordingly looking at engine statistics though. 

	Both implementations have pros and cons and going back and forth between those designs shouldn't be so hard. I use the unified approach for this implementation.

	Per Vulkan spec, the application has control of where the driver’s reserved range ends up. In this implementation, reserved range will be placed at the end of the heap buffer. So, the descriptor blob info start
	from offset 0.

	Descriptor Heap internally holds 2 buffers inside, one staging and one GPU buffer. Staging buffer is updated on the CPU and at the beginning of every frame, it is uploaded to the GPU. This way, the descriptor reads
	will be local in the GPU and more efficient.
*/

namespace SK::VkRendererBackend
{
	struct State;

	enum class ResourceDescriptorKind : uint8_t
	{
		UniformBuffer = 0,
		StorageBuffer,
		SampledImage,
		StorageImage
	};

	static constexpr uint32_t INVALID_RESOURCE_DESCRIPTOR_HANDLE = std::numeric_limits<uint32_t>::max();
	static constexpr uint32_t INVALID_SAMPLER_DESCRIPTOR_HANDLE = std::numeric_limits<uint8_t>::max();

	struct ResourceDescriptorHandle
	{
		/*
			Resource descriptor indices are absolute slots in the unified resource heap.

			This index is NOT a dense per-type index.

			For example:
				UniformBuffers[handle.index]
				StorageBuffers[handle.index]
				SampledImages[handle.index]
				StorageImages[handle.index]

			all refer to the same physical resource heap slot interpreted through different shader resource types.

			It is the caller's responsibility to use the handle with the shader resource type matching ResourceDescriptorKind.
		*/
		uint32_t index = INVALID_RESOURCE_DESCRIPTOR_HANDLE;
		ResourceDescriptorKind kind = ResourceDescriptorKind::UniformBuffer;
	};

	struct SamplerDescriptorHandle
	{
		/*
			Sampler descriptor indices are absolute slots in the sampler heap.
		*/
		uint8_t index = INVALID_SAMPLER_DESCRIPTOR_HANDLE;
	};

	inline bool isValid(ResourceDescriptorHandle handle)
	{
		return handle.index != INVALID_RESOURCE_DESCRIPTOR_HANDLE;
	}

	inline bool isValid(SamplerDescriptorHandle handle)
	{
		return handle.index != INVALID_SAMPLER_DESCRIPTOR_HANDLE;
	}

	struct DescriptorHeapDesc
	{
		uint32_t maxResourceDescriptors = 8192;
		uint32_t maxSamplerDescriptors = 256;
	};

	struct DescriptorHeap
	{
		AllocatedBuffer resourceHeapBufferStaging{};
		AllocatedBuffer samplerHeapBufferStaging{};

		AllocatedBuffer resourceHeapBufferGPU{};
		AllocatedBuffer samplerHeapBufferGPU{};
			
		uint8_t* resourceHeapStagingMappedData;
		uint8_t* samplerHeapStagingMappedData;

		VkDeviceSize resourceHeapSize = 0;
		VkDeviceSize samplerHeapSize = 0;

		VkDeviceSize resourceReservedRangeOffset = 0;
		VkDeviceSize resourceReservedRangeSize = 0;

		VkDeviceSize samplerReservedRangeOffset = 0;
		VkDeviceSize samplerReservedRangeSize = 0;

		VkDeviceSize bufferDescriptorSize = 0;
		VkDeviceSize imageDescriptorSize = 0;
		VkDeviceSize samplerDescriptorSize = 0;

		VkDeviceSize resourceDescriptorStride = 0;
		VkDeviceSize samplerDescriptorStride = 0;

		uint32_t maxResourceDescriptors = 0;
		uint32_t maxSamplerDescriptors = 0;

		uint32_t nextResourceDescriptor = 0;
		uint32_t nextSamplerDescriptor = 0;

		std::vector<uint32_t> resourceFreeList{};
		std::vector<uint8_t> samplerFreeList{};

		bool initialized = false;

		void init(State* vkRendererBackend, const DescriptorHeapDesc& desc);
		void destroy(State* vkRendererBackend);
		void update(State* vkRendererBackend);
		void upload(State* vkRendererBackend, VkCommandBuffer cmd);
		void bind(State* vkRendererBackend, VkCommandBuffer cmd);
		// Allocate functions allocates a slot in the heap and returns a handle. The descriptor info is then written with write functions with that handle to that heap slot.
		ResourceDescriptorHandle allocateResourceDescriptor(ResourceDescriptorKind kind);
		void deleteDescriptor(ResourceDescriptorHandle& handle);
		SamplerDescriptorHandle allocateSamplerDescriptor();
		void deleteDescriptor(SamplerDescriptorHandle& handle);
		VkDeviceSize getResourceDescriptorOffset(const ResourceDescriptorHandle handle);
		VkDeviceSize getSamplerDescriptorOffset(const SamplerDescriptorHandle handle);
		void writeUniformBufferDescriptor(
			ResourceDescriptorHandle handle,
			const AllocatedBuffer& buffer,
			VkDeviceSize offset,
			VkDeviceSize range
		);
		void writeStorageBufferDescriptor(
			ResourceDescriptorHandle handle,
			const AllocatedBuffer& buffer,
			VkDeviceSize offset,
			VkDeviceSize range
		);
		void writeSampledImageDescriptor(
			ResourceDescriptorHandle handle,
			const VkImageViewCreateInfo& viewInfo,
			VkImageLayout layout
		);
		void writeStorageImageDescriptor(
			ResourceDescriptorHandle handle,
			const VkImageViewCreateInfo& viewInfo,
			VkImageLayout layout
		);
		void writeSamplerDescriptor(
			SamplerDescriptorHandle handle,
			const VkSamplerCreateInfo& samplerInfo
		);
	};
};