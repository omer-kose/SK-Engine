#pragma once

#include <Renderer/RenderContext.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <typeindex>
#include <any>
#include <unordered_map>
#include <variant>
#include <vector>

namespace SK::Renderer
{
	using FrameGraphPassIndex = uint32_t;
	using FrameGraphResourceIndex = uint32_t;

	static constexpr FrameGraphPassIndex INVALID_FRAME_GRAPH_PASS = UINT32_MAX;
	static constexpr FrameGraphResourceIndex INVALID_FRAME_GRAPH_RESOURCE = UINT32_MAX;

	struct FrameGraphResourceHandle
	{
		FrameGraphResourceIndex index = INVALID_FRAME_GRAPH_RESOURCE;

		bool isValid() const
		{
			return index != INVALID_FRAME_GRAPH_RESOURCE;
		}
	};

	/*
		API-agnostic semantic resource states.
	*/
	enum class FrameGraphResourceState : uint8_t
	{
		// no tracked prior access
		Undefined = 0,

		// Texture attachment use
		ColorAttachment,
		DepthStencilAttachment,

		// Shader use: texture or buffer
		ShaderRead,
		StorageRead,
		StorageWrite,
		StorageReadWrite,

		// Texture or buffer copy/blit use
		TransferRead,
		TransferWrite,

		// Buffer-only use
		VertexBuffer,
		IndexBuffer,
		UniformBuffer,
		IndirectBuffer,

		// Texture-only external final state
		Present,
	};

	/*
		A resource is either a texture or a buffer.

		std::variant makes the type explicit while avoiding the wasted
		memory that would result from storing both descriptions/handles.
	*/
	using FrameGraphResourceDesc = std::variant<TextureDesc, BufferDesc>;
	using FrameGraphResourceBackendHandle = std::variant<TextureHandle, BufferHandle>; // the actual resource handle into the render context (thus to the backend handle)

	struct FrameGraphResourceVersion
	{
		FrameGraphPassIndex writerPass = INVALID_FRAME_GRAPH_PASS;

		std::vector<FrameGraphPassIndex> readerPasses;

		bool hasWriter() const
		{
			return writerPass != INVALID_FRAME_GRAPH_PASS;
		}
	};

	/*
		Resource entries owns description, backend handle, versions and current state of a resource.
		
		Pass-local usage details belong to FrameGraphResourceUsage, not here.
	*/
	struct FrameGraphResourceEntry
	{
		FrameGraphResourceDesc desc{};
		FrameGraphResourceBackendHandle backendHandle{};

		std::vector<FrameGraphResourceVersion> versions;

		/*
			Initialized when imported. Mutated while compiling the graph. 
		*/
		FrameGraphResourceState currentState = FrameGraphResourceState::Undefined;

		/*
			Needed for shader-resource barrier lowering. Attachment, transfer, vertex/index buffer and present states usually use None.
		*/
		ShaderStageFlags currentShaderStages = SK::Renderer::toShaderStageFlags(ShaderStageFlagBits::None);

		/*
			Optional required state after the graph is done. This is mainly used by the imported resources which must finished in a certain state (such as swapchain must finish in Present state).
		*/
		std::optional<FrameGraphResourceState> finalState = std::nullopt;

		bool imported = false;
		const char* debugName = nullptr;
	};

	/*
		A resource access declared by one specific pass.

		The graph needs this information because the same resource may be used in different states/stages by different passes.
	*/
	struct FrameGraphResourceUsage
	{
		FrameGraphResourceHandle resource{};
		FrameGraphResourceState state = FrameGraphResourceState::Undefined;
		ShaderStageFlags shaderStages = SK::Renderer::toShaderStageFlags(ShaderStageFlagBits::None);
	};

	/*
		A precomputed resource transition emitted before the pass at the corresponding compile-time plan order index.
	*/
	struct FrameGraphBarrier
	{
		FrameGraphResourceHandle resource{};

		FrameGraphResourceState before = FrameGraphResourceState::Undefined;
		FrameGraphResourceState after = FrameGraphResourceState::Undefined;

		ShaderStageFlags beforeShaderStages = SK::Renderer::toShaderStageFlags(ShaderStageFlagBits::None);
		ShaderStageFlags afterShaderStages = SK::Renderer::toShaderStageFlags(ShaderStageFlagBits::None);
	};

	struct FrameGraph;
	struct FrameGraphPassBuilder;
	struct FrameGraphPassContext;

	using FrameGraphPassExecuteFn = std::function<void(const FrameGraphPassContext&)>;

	/*
		Declared graphics/compute/copy pass.

	*/
	struct FrameGraphPass
	{
		const char* name = nullptr;
		FrameGraphPassExecuteFn execute{};

		std::vector<FrameGraphResourceUsage> reads;
		std::vector<FrameGraphResourceUsage> writes;
		std::vector<FrameGraphResourceUsage> readWrites;

		// dependsOn intentionally stores raw, potentially duplicated prerequisites.buildEdges() deduplicates them once per compilation.
		std::vector<FrameGraphPassIndex> dependsOn;
		std::vector<FrameGraphPassIndex> successors;

		uint32_t inDegree = 0;

		bool alive = false;
		/*
			A semantic property of the pass describing that the pass produces externally observable result(s) that is consumed outside this graph.
			Passes with side effect are implicit roots for culling even though nothing inside the graph reads their outputs (e.g. blit/present to swapchain, readback, cross-frame persistent resources).

			Setting side-effect properly is cruical for culling logic because culling logic treats side-effect passes alive (as roots) and then propagates liveness backward through edges.
		*/
		bool hasSideEffect = false;
		/*
			Brute-force override to keep a pass alive exempt from culling logic. Examples for such passes: GPU Timestamp/profiling pass, a query dispatch, a debug/validation pass. 
		*/
		bool neverCull = false;
	};

	struct FrameGraphBlackboardEntry
	{
		std::type_index type = typeid(void);
		const char* debugName = nullptr;
		std::any value{};
	};

	/*
		Strictly typed dynamic blackboard allowing communication between passes for resources. 

		Each resource must be strictly typed before being published into the blackboard. The type of the resource is unique and the key to find the resource in the blackboard.

		The entries are not directly stored inside the unordered_map but inside contiguous memory. Clearing unordered_map requires traversing buckets and deleting the entries on individiual heap nodes. 
		This causes heavy heap fragmentation. Meanwhile, clearing a vector is virtually free when it holds trivial types (typed blackboard entries are just packed up resource handles in a struct). Crucially, the vector
		retains its allocated capacity for the next frame. This difference is critical as the frame graph and the blackboard are cleared up per-frame.

		Also, having the entries contiguously makes the blackboard lightweight to work with possible debugging tools.
	*/
	struct FrameGraphBlackboard
	{
		std::vector<FrameGraphBlackboardEntry> entries;
		std::unordered_map<std::type_index, uint32_t> entryIndexByType;

		template<typename T>
		void add(const T& value, const char* debugName = nullptr)
		{
			const std::type_index type = typeid(T);
			assert(entryIndexByType.contains(type) == false && "A frame-graph blackboard entry of this type was already published.");

			const uint32_t entryIndex = static_cast<uint32_t>(entries.size());
			entries.push_back({
				.type = type,
				.debugName = debugName,
				.value = value
			});

			entryIndexByType[type] = entryIndex;
		}

		template<typename T>
		bool contains() const
		{
			return entryIndexByType.contains(typeid(T));
		}

		template<typename T>
		T& get()
		{
			const auto found = entryIndexByType.find(typeid(T));
			assert(found != entryIndexByType.end() && "Required frame-graph blackboard entry was not published.");

			return std::any_cast<T&>(entries[found->second].value);
		}

		template<typename T>
		const T& get()
		{
			const auto found = entryIndexByType.find(typeid(T));
			assert(found != entryIndexByType.end() && "Required frame-graph blackboard entry was not published.");

			return std::any_cast<const T&>(entries[found->second].value);
		}

		template<typename T>
		T* tryGet()
		{
			const auto found = entryIndexByType.find(typeid(T));
			if (found == entryIndexByType.end())
			{
				return nullptr;
			}

			return std::any_cast<T>(&entries[found->second].value);
		}

		template<typename T>
		const T* tryGet()
		{
			const auto found = entryIndexByType.find(typeid(T));
			if (found == entryIndexByType.end())
			{
				return nullptr;
			}

			return std::any_cast<const T>(&entries[found->second].value);
		}

		void clear();
	};

	struct FrameGraphCompiledPlan
	{
		std::vector<FrameGraphPassIndex> sortedPasses;
		// barriers[orderIndex] belongs to sortedPasses[orderIndex].
		std::vector<std::vector<FrameGraphBarrier>> barriers;
		std::vector<FrameGraphBarrier> finalBarriers;
		
		void clear();
	};

	struct FrameGraphPassContext
	{
		RenderContext* renderContext = nullptr;
		const FrameGraph* frameGraph = nullptr;

		FrameGraphPassIndex passIndex = INVALID_FRAME_GRAPH_PASS;
		
		TextureHandle getTexture(FrameGraphResourceHandle resource) const;
		BufferHandle getBuffer(FrameGraphResourceHandle resource) const;
	};

	/*
		Temporary declaration helper created by addPass().

		FrameGraphPassBuilder is not retained.
	*/
	struct FrameGraphPassBuilder
	{
		FrameGraph* frameGraph;

		FrameGraphPassIndex passIndex = INVALID_FRAME_GRAPH_PASS;

		void readTexture(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages);
		void readBuffer(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages);

		void writeTexture(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages = SK::Renderer::toShaderStageFlags(ShaderStageFlagBits::None));
		void writeBuffer(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages = SK::Renderer::toShaderStageFlags(ShaderStageFlagBits::None));

		void readWriteTexture(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages);
		void readWriteBuffer(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages);

		// Marks this pass as an implicit root for culling
		void setSideEffect();
		// Marks this pass to be never culled regardless of graph reasoning.
		void setNeverCull();
	};

	struct FrameGraph
	{
		std::vector<FrameGraphPass> passes;
		std::vector<FrameGraphResourceEntry> resourceEntries;

		FrameGraphBlackboard blackboard{};
		FrameGraphCompiledPlan compiledPlan;

		bool compiled = false;

		void clear();

		FrameGraphResourceHandle importTexture(TextureHandle texture, const TextureDesc& desc, FrameGraphResourceState initialState, std::optional<FrameGraphResourceState> finalState, const char* debugName);
		FrameGraphResourceHandle importBuffer(BufferHandle buffer, const BufferDesc& desc, FrameGraphResourceState initialState, std::optional<FrameGraphResourceState> finalState, const char* debugName);

		template<typename SetupFn, typename ExecuteFn>
		FrameGraphPassIndex addPass(const char* name, SetupFn&& setup, ExecuteFn&& execute)
		{
			assert(compiled == false);
			assert(name != nullptr);

			const FrameGraphPassIndex passIndex = static_cast<FrameGraphPassIndex>(passes.size());
			passes.push_back({
				.name = name,
				.execute = std::forward<ExecuteFn>(execute)
			});

			FrameGraphPassBuilder builder{};
			builder.frameGraph = this;
			builder.passIndex = passIndex;
		
			/*
				Setup is always invoked immediately and never retained. Capturing graph-building local variables by reference is therefore safe.
			*/
			std::forward<SetupFn>(setup)(builder);

			return passIndex;
		}

		bool compile(RenderContext* renderContext);
		void execute(RenderContext* renderContext);

		// Internal Helpers
		friend struct FrameGraphPassBuilder;

		void addDependency(FrameGraphPassIndex dependentPass, FrameGraphPassIndex prerequisitePass);
		void buildEdges();
		bool topologicalSort(std::vector<FrameGraphPassIndex>& sorted) const;
		void cull(const std::vector<FrameGraphPassIndex>& sorted);
		bool validateUsage(const FrameGraphResourceUsage& usage, bool reads, bool writes) const; // used by computeBarriers for debugging purposes
		void computeBarriers(const std::vector<FrameGraphPassIndex>& sorted);
	};

};