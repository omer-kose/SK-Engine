#include "FrameGraph.h"

#include <queue>

static bool isTextureResource(const SK::Renderer::FrameGraphResourceEntry& entry)
{
	return std::holds_alternative<SK::Renderer::TextureHandle>(entry.backendHandle);
}

static bool isBufferResource(const SK::Renderer::FrameGraphResourceEntry& entry)
{
	return std::holds_alternative<SK::Renderer::BufferHandle>(entry.backendHandle);
}

static bool isTextureState(SK::Renderer::FrameGraphResourceState state)
{
    using State = SK::Renderer::FrameGraphResourceState;

    switch (state)
    {
    case State::ColorAttachment:
    case State::DepthStencilAttachment:
    case State::ShaderRead:
    case State::StorageRead:
    case State::StorageWrite:
    case State::StorageReadWrite:
    case State::TransferRead:
    case State::TransferWrite:
    case State::Present:
        return true;

    default:
        return false;
    }
}

static bool isBufferState(SK::Renderer::FrameGraphResourceState state)
{
    using State = SK::Renderer::FrameGraphResourceState;

    switch (state)
    {
    case State::ShaderRead:
    case State::StorageRead:
    case State::StorageWrite:
    case State::StorageReadWrite:
    case State::TransferRead:
    case State::TransferWrite:
    case State::VertexBuffer:
    case State::IndexBuffer:
    case State::UniformBuffer:
    case State::IndirectBuffer:
        return true;

    default:
        return false;
    }
}

static bool stateHasWriteAccess(SK::Renderer::FrameGraphResourceState state)
{
    using State = SK::Renderer::FrameGraphResourceState;

    switch (state)
    {
    case State::ColorAttachment:
    case State::DepthStencilAttachment:
    case State::StorageWrite:
    case State::StorageReadWrite:
    case State::TransferWrite:
        return true;

    default:
        return false;
    }
}

static bool barrierRequired(const SK::Renderer::FrameGraphResourceEntry& entry, const SK::Renderer::FrameGraphResourceUsage& usage)
{
    /*
        State transitions always require a barrier.
        A write also requires a memory dependency even when the semantic state remains unchanged.
    */
    if (entry.currentState != usage.state)
    {
        return true;
    }

    if (stateHasWriteAccess(entry.currentState) || stateHasWriteAccess(usage.state))
    {
        return true;
    }

    return false;
}

static void appendReadUsage(
    SK::Renderer::FrameGraph* frameGraph,
    SK::Renderer::FrameGraphPassIndex passIndex,
    SK::Renderer::FrameGraphResourceHandle resource,
    SK::Renderer::FrameGraphResourceState state,
    SK::Renderer::ShaderStageFlags shaderStages
)
{
    assert(frameGraph);
    assert(resource.isValid());
    assert(passIndex < frameGraph->passes.size());
    assert(resource.index < frameGraph->resourceEntries.size());

    SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    SK::Renderer::FrameGraphResourceVersion& currentVersion = entry.versions.back();

    /*
        RAW dependency:
        Writer must finish before this reader.
    */
    if (currentVersion.hasWriter())
    {
        frameGraph->addDependency(passIndex, currentVersion.writerPass);
    }

    currentVersion.readerPasses.push_back(passIndex);

    frameGraph->passes[passIndex].reads.push_back({
        .resource = resource,
        .state = state,
        .shaderStages = shaderStages
    });
}

static SK::Renderer::FrameGraphResourceHandle appendWriteUsage(
    SK::Renderer::FrameGraph* frameGraph,
    SK::Renderer::FrameGraphPassIndex passIndex,
    SK::Renderer::FrameGraphResourceHandle resource,
    SK::Renderer::FrameGraphResourceState state,
    SK::Renderer::ShaderStageFlags shaderStages,
    bool readWrite
)
{
    assert(frameGraph);
    assert(resource.isValid());
    assert(passIndex < frameGraph->passes.size());
    assert(resource.index < frameGraph->resourceEntries.size());

    SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    SK::Renderer::FrameGraphResourceVersion& previousVersion = entry.versions.back();

    /*
        WAW dependency:
        Previous writer must finish before this writer.
    */
    if (previousVersion.hasWriter())
    {
        frameGraph->addDependency(passIndex, previousVersion.writerPass);
    }

    /*
        WAR dependency:
        All readers of the previous version must finish before this writer bumps up that version.
    */
    for (const SK::Renderer::FrameGraphPassIndex readerPass : previousVersion.readerPasses)
    {
        frameGraph->addDependency(passIndex, readerPass);
    }

    entry.versions.push_back({
        .writerPass = passIndex
    });

    const SK::Renderer::FrameGraphResourceUsage usage{
        .resource = resource,
        .state = state,
        .shaderStages = shaderStages
    };

    if (readWrite)
    {
        frameGraph->passes[passIndex].readWrites.push_back(usage);
    }
    else
    {
        frameGraph->passes[passIndex].writes.push_back(usage);
    }

    return resource;
}

static void appendUniqueUse(std::vector<SK::Renderer::FrameGraphResourceUsage>& usages, const SK::Renderer::FrameGraphResourceUsage& usage)
{
    for (const SK::Renderer::FrameGraphResourceUsage& existing : usages)
    {
        assert(existing.resource.index != usage.resource.index &&
            "A resource may only be declared once per pass. "
            "Use readWriteTexture/readWriteBuffer when one pass "
            "requires both read and write access.");
    }

    usages.push_back(usage);
}

void SK::Renderer::FrameGraphBlackboard::clear()
{
    entries.clear();
    entryIndexByType.clear();
}

void SK::Renderer::FrameGraphCompiledPlan::clear()
{
    sortedPasses.clear();
    barriers.clear();
    finalBarriers.clear();
}

SK::Renderer::TextureHandle SK::Renderer::FrameGraphPassContext::getTexture(FrameGraphResourceHandle resource) const
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry entry = frameGraph->resourceEntries[resource.index];
    assert(isTextureResource(entry));

    return std::get<TextureHandle>(entry.backendHandle);
}

SK::Renderer::BufferHandle SK::Renderer::FrameGraphPassContext::getBuffer(FrameGraphResourceHandle resource) const
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry entry = frameGraph->resourceEntries[resource.index];
    assert(isBufferResource(entry));

    return std::get<BufferHandle>(entry.backendHandle);
}

void SK::Renderer::FrameGraphPassBuilder::readTexture(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages)
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    assert(isTextureResource(entry.backendHandle));

    appendReadUsage(frameGraph, passIndex, resource, state, shaderStages);
}

void SK::Renderer::FrameGraphPassBuilder::readBuffer(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages)
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    assert(isBufferResource(entry.backendHandle));

    appendReadUsage(frameGraph, passIndex, resource, state, shaderStages);
}

void SK::Renderer::FrameGraphPassBuilder::writeTexture(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages)
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    assert(isTextureResource(entry.backendHandle));

    appendWriteUsage(frameGraph, passIndex, resource, state, shaderStages, false);
}

void SK::Renderer::FrameGraphPassBuilder::writeBuffer(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages)
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    assert(isBufferResource(entry.backendHandle));

    appendWriteUsage(frameGraph, passIndex, resource, state, shaderStages, false);
}

void SK::Renderer::FrameGraphPassBuilder::readWriteTexture(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages)
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    assert(isTextureResource(entry.backendHandle));

    appendWriteUsage(frameGraph, passIndex, resource, state, shaderStages, true);
}

void SK::Renderer::FrameGraphPassBuilder::readWriteBuffer(FrameGraphResourceHandle resource, FrameGraphResourceState state, ShaderStageFlags shaderStages)
{
    assert(frameGraph != nullptr);
    assert(resource.isValid());
    assert(resource.index < frameGraph->entries.size());

    const SK::Renderer::FrameGraphResourceEntry& entry = frameGraph->resourceEntries[resource.index];
    assert(isBufferResource(entry.backendHandle));

    appendWriteUsage(frameGraph, passIndex, resource, state, shaderStages, true);
}

void SK::Renderer::FrameGraphPassBuilder::setSideEffect()
{
    assert(frameGraph != nullptr);
    assert(passIndex < frameGraph->passes.size());

    frameGraph->passes[passIndex].hasSideEffect = true;
}

void SK::Renderer::FrameGraphPassBuilder::setNeverCull()
{
    assert(frameGraph != nullptr);
    assert(passIndex < frameGraph->passes.size());

    frameGraph->passes[passIndex].neverCull = true;
}

void SK::Renderer::FrameGraph::init(uint32_t frameIndex_)
{
    frameIndex = frameIndex_;
}

void SK::Renderer::FrameGraph::clear()
{
    passes.clear();
    resourceEntries.clear();

    blackboard.clear();
    compiledPlan.clear();

    compiled = false;
}

SK::Renderer::FrameGraphResourceHandle SK::Renderer::FrameGraph::importTexture(TextureHandle texture, const TextureDesc& desc, FrameGraphResourceState initialState, std::optional<FrameGraphResourceState> finalState, const char* debugName)
{
    assert(compiled == false);
    assert(texture.id != INVALID_HANDLE);
    assert(debugName != nullptr);
    assert(isTextureState(initialState));
    assert(finalState.has_value() ? isTextureResource(finalState.value()) : true);

    const SK::Renderer::FrameGraphResourceIndex resourceIndex = static_cast<SK::Renderer::FrameGraphResourceIndex>(resourceEntries.size());

    SK::Renderer::FrameGraphResourceEntry entry{};
    entry.desc = desc;
    entry.backendHandle = texture;
    entry.versions.push_back({});
    entry.currentState = initialState;
    entry.currentShaderStages = static_cast<SK::Renderer::ShaderStageFlags>(SK::Renderer::ShaderStageFlagBits::None);
    entry.finalState = finalState;
    entry.imported = true;
    entry.debugName = debugName;

    resourceEntries.push_back(std::move(entry));

    return { resourceIndex };
}

SK::Renderer::FrameGraphResourceHandle SK::Renderer::FrameGraph::importBuffer(BufferHandle buffer, const BufferDesc& desc, FrameGraphResourceState initialState, std::optional<FrameGraphResourceState> finalState, const char* debugName)
{
    assert(compiled == false);
    assert(texture.id != INVALID_HANDLE);
    assert(debugName != nullptr);
    /*
        Differently from texture resources, undefined state is allowed as the initial buffer state. This is because buffers have no layouts. While, in theory, a texture could also start in an undefined state
        and could be used in a pass, if that pass requires a layout transition, the state of the texture must be known. This is because image layouts are inferred from the access flags.
    */
    assert(initialState == SK::Renderer::FrameGraphResourceState::Undefined || isBufferState(initialState));
    assert(finalState.has_value() ? isBufferResource(finalState) : true);

    const SK::Renderer::FrameGraphResourceIndex resourceIndex = static_cast<SK::Renderer::FrameGraphResourceIndex>(resourceEntries.size());

    SK::Renderer::FrameGraphResourceEntry entry{};
    entry.desc = desc;
    entry.backendHandle = buffer;
    entry.versions.push_back({});
    entry.currentState = initialState;
    entry.currentShaderStages = static_cast<SK::Renderer::ShaderStageFlags>(SK::Renderer::ShaderStageFlagBits::None);
    entry.finalState = finalState;
    entry.imported = true;
    entry.debugName = debugName;

    resourceEntries.push_back(std::move(entry));

    return { resourceIndex };
}

bool SK::Renderer::FrameGraph::compile(RenderContext* renderContext)
{
    assert(renderContext);
    assert(!compiled);

    buildEdges();

    std::vector<SK::Renderer::FrameGraphPassIndex> sorted;
    if (topologicalSort(sorted))
    {
        /*
            Cycle detected. Phase 1 returns false; later diagnostics can
            report the exact cycle and resource dependency chain.
        */
        return false;
    }

    cull(sorted);

    compiledPlan.sortedPasses = sorted;
    computeBarriers(sorted);

    compiled = true;
    return true;
}

bool SK::Renderer::FrameGraph::execute(RenderContext* renderContext)
{
    assert(renderContext);
    assert(compiled);

    assert(compiledPlan.sortedPasses.size() == compiledPlan.barriers.size());

    for (uint32_t sortedIndex = 0; sortedIndex < compiledPlan.sortedPasses.size(); ++sortedIndex)
    {
        const SK::Renderer::FrameGraphPassIndex passIndex = compiledPlan.sortedPasses[sortedIndex];
        const SK::Renderer::FrameGraphPass& pass = passes[passIndex];

        if (!pass.alive)
        {
            continue;
        }

        executeFrameGraphBarriers(renderContext, *this, compiledPlan.barriers[sortedIndex]);

        SK::Renderer::FrameGraphPassContext context{};
        context.renderContext = renderContext;
        context.frameGraph = this;
        context.passIndex = passIndex;

        pass.execute(context);
    }

    executeFrameGraphBarriers(renderContext, *this, compiledPlan.finalBarriers);
}

void SK::Renderer::FrameGraph::addDependency(FrameGraphPassIndex dependentPass, FrameGraphPassIndex prerequisitePass)
{
    assert(dependentPass < passes.size());
    assert(prerequisitePass < passes.size());

    if (dependentPass == prerequisitePass)
    {
        return;
    }

    // addDependency does not care about duplicates. Deduplication will be done while building edges (buildEdges()).
    passes[dependentPass].dependsOn.push_back(prerequisitePass);
}

void SK::Renderer::FrameGraph::buildEdges()
{
    // Generation Stamp to be used as LUT to eliminate duplicate dependencies.
    std::vector<uint32_t> seenStamps(passes.size(), 0);

    uint32_t currentStamp = 0; // No pass has the stamp 0.

    for (SK::Renderer::FrameGraphPassIndex passIndex = 0; passIndex < passes.size(); ++passIndex)
    {
        SK::Renderer::FrameGraphPass& pass = passes[passIndex];

        ++currentStamp; // stamp of this pass.

        for (const SK::Renderer::FrameGraphPassIndex dependency : pass.dependsOn)
        {
            // If this generation's stamp has already been seen by this dependency, this dependency is already processed. Skipping the duplicates.
            if (seenStamps[dependency] == currentStamp)
            {
                continue;
            }

            seenStamps[dependency] = currentStamp;

            passes[dependency].successors.push_back(passIndex);

            ++pass.inDegree;
        }
    }
}

bool SK::Renderer::FrameGraph::topologicalSort(std::vector<FrameGraphPassIndex>& sorted) const
{
    std::vector<uint32_t> inDegrees;
    inDegrees.reserve(passes.size());

    std::queue<SK::Renderer::FrameGraphPassIndex> ready;

    for (SK::Renderer::FrameGraphPassIndex passIndex = 0; passes.size(); ++passIndex)
    {
        const uint32_t degree = passes[passIndex].inDegree;
        inDegrees.push_back(degree);
        if (degree == 0)
        {
            ready.push(passIndex);
        }
    }

    sorted.reserve(passes.size());

    while (!ready.empty())
    {
        const SK::Renderer::FrameGraphPassIndex passIndex = ready.front();
        ready.pop();

        sorted.push_back(passIndex);

        for (const SK::Renderer::FrameGraphPassIndex successor : passes[passIndex].successors)
        {
            assert(inDegrees[successor] > 0);
            
            --inDegrees[successor];
            if (inDegrees[successor] == 0)
            {
                ready.push(successor);
            }
        }
    }

    return sorted.size() == passes.size();
}

void SK::Renderer::FrameGraph::cull(const std::vector<FrameGraphPassIndex>& sorted)
{
    for (SK::Renderer::FrameGraphPass& pass : passes)
    {
        pass.alive = pass.neverCull || pass.hasSideEffect;
    }

    // Propagate alive info from implicit roots backwards into the frame graph.
    for (uint32_t sortedIndex = sorted.size() - 1; sortedIndex >= 0; --sortedIndex)
    {
        const SK::Renderer::FrameGraphPassIndex passIndex = sorted[sortedIndex];

        const SK::Renderer::FrameGraphPass& pass = passes[passIndex];

        if (!pass.alive)
        {
            continue;
        }

        for (const SK::Renderer::FrameGraphPassIndex dependency : pass.dependsOn)
        {
            passes[dependency].alive = true;
        }
    }
}

bool SK::Renderer::FrameGraph::validateUsage(const FrameGraphResourceUsage& usage, bool reads, bool writes) const
{
    if (!usage.resource.isValid() || usage.resource.index >= resourceEntries.size())
    {
        return false;
    }

    /*
        Why did Undefined excluded: Undefined isn't something a pass does. No draw or dispatch operates in undefined state. It is only meaningful as a "before" value the graph compute internally
        (the imported buffer inital state or the barrier before field).

        Why did Present excluded: Present never appears as a pass usage. It only appears as a "final state". It is handled explicitly in computeBarriers() while putting final barriers. 
    */
    if (usage.state == SK::Renderer::FrameGraphResourceState::Undefined || usage.state == SK::Renderer::FrameGraphResourceState::Present)
    {
        return false;
    }

    const SK::Renderer::FrameGraphResourceEntry& entry = resourceEntries[usage.resource.index];

    if (isTextureResource(entry))
    {
        if (!isTextureState(usage.state))
        {
            return false;
        }
    }
    else
    {
        if (!isBufferState(usage.state))
        {
            return false;
        }
    }

    if (reads && writes && usage.state != SK::Renderer::FrameGraphResourceState::StorageReadWrite)
    {
        return false;
    }

    if (reads && usage.state == FrameGraphResourceState::StorageWrite)
    {
        return false;
    }

    if (writes && usage.state == FrameGraphResourceState::ShaderRead)
    {
        return false;
    }

    return true;
}

void SK::Renderer::FrameGraph::computeBarriers(const std::vector<FrameGraphPassIndex>& sorted)
{
    compiledPlan.barriers.resize(sorted.size());

    for (uint32_t sortedIndex = 0; sortedIndex < sorted.size(); ++sortedIndex)
    {
        const SK::Renderer::FrameGraphPassIndex passIndex = sorted[sortedIndex];
        const SK::Renderer::FrameGraphPass pass = passes[passIndex];
        
        if (!pass.alive)
        {
            continue;
        }

        std::vector<SK::Renderer::FrameGraphResourceUsage> usages;
        usages.resize(pass.reads.size() + pass.writes.size() + pass.readWrites.size());

        for (const SK::Renderer::FrameGraphResourceUsage& usage : pass.reads)
        {
            assert(!validateUsage(usage, true, false));
            appendUniqueUse(usages, usage);
        }

        for (const SK::Renderer::FrameGraphResourceUsage& usage : pass.writes)
        {
            assert(!validateUsage(usage, false, true));
            appendUniqueUse(usages, usage);
        }

        for (const SK::Renderer::FrameGraphResourceUsage& usage : pass.readWrites)
        {
            assert(!validateUsage(usage, true, true));
            appendUniqueUse(usages, usage);
        }

        std::vector<SK::Renderer::FrameGraphBarrier>& barriers = compiledPlan.barriers[sortedIndex];
        for (const SK::Renderer::FrameGraphResourceUsage& usage : usages)
        {
            SK::Renderer::FrameGraphResourceEntry& entry = resourceEntries[usage.resource.index];
            if (barrierRequired(entry, usage))
            {
                barriers.push_back(SK::Renderer::FrameGraphBarrier{
                    .resource = usage.resource,
                    .before = entry.currentState,
                    .after = usage.state,
                    .beforeShaderStages = entry.currentShaderStages,
                    .afterShaderStages = usage.shaderStages
                });
            }

            // Usage is processed and if necessary a barrier is put. By the frame graph simulation, now the resource is used by the "usage". Update the resource entry to track the current usage of the resource.
            entry.currentState = usage.state;
            entry.currentShaderStages = usage.shaderStages;
        }
    }

    for (SK::Renderer::FrameGraphResourceIndex resourceIndex = 0; resourceIndex < resourceEntries.size(); ++resourceIndex)
    {
        SK::Renderer::FrameGraphResourceEntry& entry = resourceEntries[resourceIndex];

        if (!entry.imported || !entry.finalState.has_value())
        {
            continue;
        }

        const SK::Renderer::FrameGraphResourceState requiredState = entry.finalState.value();
        const bool requiresFinalBarrier = entry.currentState != requiredState || stateHasWriteAccess(entry.currentState);

        if (!requiresFinalBarrier)
        {
            continue;
        }

        compiledPlan.finalBarriers.push_back(SK::Renderer::FrameGraphBarrier{
            .resource = { resourceIndex },
            .before = entry.currentState,
            .after = requiredState,
            .beforeShaderStages = entry.currentShaderStages,
            .afterShaderStages = static_cast<SK::Renderer::ShaderStageFlags>(SK::Renderer::ShaderStageFlagBits::None)
        });

        entry.currentState = requiredState;
        entry.currentShaderStages = static_cast<SK::Renderer::ShaderStageFlags>(SK::Renderer::ShaderStageFlagBits::None);
    }
}