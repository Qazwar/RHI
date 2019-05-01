#include "CommandContextVk.h"
#include "DeferredContextVk.h"
#include "DeviceVk.h"
#include "PipelineVk.h"
#include "RenderPassVk.h"
#include "SamplerVk.h"
#include "VkHelpers.h"
#include <cstring>
#include <unordered_set>

namespace RHI
{

void CCommandContextVk::Convert(VkOffset3D& dst, const COffset3D& src)
{
    static_assert(sizeof(VkOffset3D) == sizeof(COffset3D), "struct size mismatch");
    dst = reinterpret_cast<const VkOffset3D&>(src);
}

void CCommandContextVk::Convert(VkExtent3D& dst, const CExtent3D& src)
{
    static_assert(sizeof(VkExtent3D) == sizeof(CExtent3D), "struct size mismatch");
    dst = reinterpret_cast<const VkExtent3D&>(src);
}

void CCommandContextVk::Convert(VkImageSubresourceLayers& dst, const CImageSubresourceLayers& src)
{
    dst.aspectMask = VkImageAspectFlagBits::VK_IMAGE_ASPECT_COLOR_BIT; // TODO:
    dst.baseArrayLayer = src.BaseArrayLayer;
    dst.layerCount = src.LayerCount;
    dst.mipLevel = src.MipLevel;
}

void CCommandContextVk::Convert(VkImageCopy& dst, const CImageCopy& src)
{
    Convert(dst.srcSubresource, src.SrcSubresource);
    Convert(dst.srcOffset, src.SrcOffset);
    Convert(dst.dstSubresource, src.DstSubresource);
    Convert(dst.dstOffset, src.DstOffset);
    Convert(dst.extent, src.Extent);
}

void CCommandContextVk::Convert(VkImageResolve& dst, const CImageResolve& src)
{
    Convert(dst.srcSubresource, src.SrcSubresource);
    Convert(dst.srcOffset, src.SrcOffset);
    Convert(dst.dstSubresource, src.DstSubresource);
    Convert(dst.dstOffset, src.DstOffset);
    Convert(dst.extent, src.Extent);
}

void CCommandContextVk::Convert(VkBufferImageCopy& dst, const CBufferImageCopy& src)
{
    dst.bufferOffset = src.BufferOffset;
    dst.bufferRowLength = src.BufferRowLength;
    dst.bufferImageHeight = src.BufferImageHeight;
    Convert(dst.imageSubresource, src.ImageSubresource);
    Convert(dst.imageOffset, src.ImageOffset);
    Convert(dst.imageExtent, src.ImageExtent);
}

void CCommandContextVk::Convert(VkImageBlit& dst, const CImageBlit& src)
{
    Convert(dst.srcSubresource, src.SrcSubresource);
    Convert(dst.srcOffsets[0], src.SrcOffsets[0]);
    Convert(dst.srcOffsets[1], src.SrcOffsets[1]);
    Convert(dst.dstSubresource, src.DstSubresource);
    Convert(dst.dstOffsets[0], src.DstOffsets[0]);
    Convert(dst.dstOffsets[1], src.DstOffsets[1]);
}

CCommandContextVk::CCommandContextVk(CDeviceVk& p, EQueueType queueType, ECommandContextKind kind)
    : Parent(p)
    , QueueType(queueType)
    , Kind(kind)
{
    BeginBuffer();
}

CCommandContextVk::~CCommandContextVk()
{
    EndBuffer();
    vkDestroySemaphore(Parent.GetVkDevice(), SignalSemaphore, nullptr);
}

void CCommandContextVk::BeginBuffer()
{
    if (Kind != ECommandContextKind::Immediate)
    {
        CmdBuffer =
            Parent.GetSubmissionTracker().GetTransientPool(QueueType)->AllocateCommandBuffer();
    }
    else
    {
        CmdBuffer = Parent.GetSubmissionTracker()
                        .GetCurrentFrameResources()
                        .FrameCmdPool->AllocateCommandBuffer();
    }

    CmdBuffer->BeginRecording(nullptr, 0);

    AccessTracker.Clear();
    WaitSemaphores.clear();
    WaitStages.clear();
    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK(vkCreateSemaphore(Parent.GetVkDevice(), &semaphoreInfo, nullptr, &SignalSemaphore));

    DeferredDeleters.clear();
}

void CCommandContextVk::EndBuffer()
{
    CmdBuffer->EndRecording();
    CurrBindings.Reset();
}

void CCommandContextVk::TransitionImage(CImage& image, EResourceState newState)
{
    auto& imageImpl = static_cast<CImageVk&>(image);
    CImageSubresourceRange range;
    range.BaseArrayLayer = 0;
    range.BaseMipLevel = 0;
    range.LayerCount = imageImpl.GetArrayLayers();
    range.LevelCount = imageImpl.GetMipLevels();
    AccessTracker.TransitionImageState(CmdBuffer->GetHandle(), &imageImpl, range, newState,
                                       QueueType == QT_TRANSFER);
}

void CCommandContextVk::CopyBuffer(CBuffer& src, CBuffer& dst,
                                   const std::vector<CBufferCopy>& regions)
{
    static_assert(sizeof(CBufferCopy) == sizeof(VkBufferCopy), "struct size mismatch");
    const VkBufferCopy* r = reinterpret_cast<const VkBufferCopy*>(regions.data());

    vkCmdCopyBuffer(CmdBuffer->GetHandle(), static_cast<CBufferVk&>(src).Buffer,
                    static_cast<CBufferVk&>(dst).Buffer, static_cast<uint32_t>(regions.size()), r);
}

void CCommandContextVk::CopyImage(CImage& src, CImage& dst, const std::vector<CImageCopy>& regions)
{
    std::vector<VkImageCopy> r;
    for (const auto& rs : regions)
    {
        VkImageCopy next;
        Convert(next, rs);
        r.push_back(next);
    }
    TransitionImage(src, EResourceState::CopySource);
    TransitionImage(dst, EResourceState::CopyDest);
    auto& srcImpl = static_cast<CImageVk&>(src);
    auto& dstImpl = static_cast<CImageVk&>(dst);
    vkCmdCopyImage(CmdBuffer->GetHandle(), srcImpl.GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImpl.GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(r.size()), r.data());
}

void CCommandContextVk::CopyBufferToImage(CBuffer& src, CImage& dst,
                                          const std::vector<CBufferImageCopy>& regions)
{
    std::vector<VkBufferImageCopy> vkregions;
    for (const auto& rs : regions)
    {
        VkBufferImageCopy next;
        Convert(next, rs);
        vkregions.push_back(next);
    }
    TransitionImage(dst, EResourceState::CopyDest);
    auto& dstImpl = static_cast<CImageVk&>(dst);
    vkCmdCopyBufferToImage(CmdBuffer->GetHandle(), static_cast<CBufferVk&>(src).Buffer,
                           dstImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(vkregions.size()), vkregions.data());
}

void CCommandContextVk::CopyImageToBuffer(CImage& src, CBuffer& dst,
                                          const std::vector<CBufferImageCopy>& regions)
{
    std::vector<VkBufferImageCopy> vkregions;
    for (const auto& rs : regions)
    {
        VkBufferImageCopy next;
        Convert(next, rs);
        vkregions.push_back(next);
    }
    TransitionImage(src, EResourceState::CopySource);
    auto& srcImpl = static_cast<CImageVk&>(src);
    vkCmdCopyImageToBuffer(CmdBuffer->GetHandle(), srcImpl.GetVkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<CBufferVk&>(dst).Buffer,
                           static_cast<uint32_t>(vkregions.size()), vkregions.data());
}

void CCommandContextVk::BlitImage(CImage& src, CImage& dst, const std::vector<CImageBlit>& regions,
                                  EFilter filter)
{
    std::vector<VkImageBlit> r;
    for (const auto& rs : regions)
    {
        VkImageBlit next;
        Convert(next, rs);
        r.push_back(next);
    }
    TransitionImage(src, EResourceState::CopySource);
    TransitionImage(dst, EResourceState::CopyDest);
    auto& srcImpl = static_cast<CImageVk&>(src);
    auto& dstImpl = static_cast<CImageVk&>(dst);
    vkCmdBlitImage(CmdBuffer->GetHandle(), srcImpl.GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImpl.GetVkImage(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(r.size()), r.data(),
                   VkCast(filter));
}

void CCommandContextVk::ResolveImage(CImage& src, CImage& dst,
                                     const std::vector<CImageResolve>& regions)
{
    std::vector<VkImageResolve> r;
    for (const auto& rs : regions)
    {
        VkImageResolve next;
        Convert(next, rs);
        r.push_back(next);
    }
    TransitionImage(src, EResourceState::CopySource);
    TransitionImage(dst, EResourceState::CopyDest);
    auto& srcImpl = static_cast<CImageVk&>(src);
    auto& dstImpl = static_cast<CImageVk&>(dst);
    vkCmdResolveImage(CmdBuffer->GetHandle(), srcImpl.GetVkImage(),
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImpl.GetVkImage(),
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(r.size()),
                      r.data());
}

void CCommandContextVk::FinishRecording()
{
    throw CRHIException("Can't call this on an immediate context");
}

void CCommandContextVk::BindPipeline(CPipeline& pipeline)
{
    CurrPipeline = static_cast<CPipelineVk*>(&pipeline);
    vkCmdBindPipeline(CmdBuffer->GetHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                      CurrPipeline->GetHandle());

    // Bind the default viewport and scissors
    auto renderArea = CurrRenderPass->GetArea();
    VkViewport vp;
    vp.x = 0;
    vp.y = 0;
    vp.width = static_cast<float>(renderArea.extent.width);
    vp.height = static_cast<float>(renderArea.extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(CmdBuffer->GetHandle(), 0, 1, &vp);
    vkCmdSetScissor(CmdBuffer->GetHandle(), 0, 1, &renderArea);
    const float blendFactor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    vkCmdSetBlendConstants(CmdBuffer->GetHandle(), blendFactor);
    vkCmdSetStencilReference(CmdBuffer->GetHandle(), VK_STENCIL_FRONT_AND_BACK, 0);
}

void CCommandContextVk::BindBuffer(CBuffer& buffer, size_t offset, size_t range, uint32_t set,
                                   uint32_t binding, uint32_t index)
{
    auto& bufferImpl = static_cast<CBufferVk&>(buffer);
    CurrBindings.BindBuffer(bufferImpl.Buffer, offset, range, set, binding, index);
}

void CCommandContextVk::BindBufferView(CBufferView& bufferView, uint32_t set, uint32_t binding,
                                       uint32_t index)
{
    throw "unimplemented";
}

void CCommandContextVk::BindConstants(const void* pData, size_t size, uint32_t set,
                                      uint32_t binding, uint32_t index)
{
    auto* bufferImpl = Parent.GetHugeConstantBuffer();
    size_t offset;
    size_t minAlignment = Parent.GetVkLimits().minUniformBufferOffsetAlignment;
    void* bufferData = bufferImpl->Allocate(size, minAlignment, offset);
    memcpy(bufferData, pData, size);
    CurrBindings.BindBuffer(bufferImpl->GetHandle(), offset, size, set, binding, index);
}

void CCommandContextVk::BindImageView(CImageView& imageView, uint32_t set, uint32_t binding,
                                      uint32_t index)
{
    auto& impl = static_cast<CImageViewVk&>(imageView);
    CurrBindings.BindImageView(&impl, VK_NULL_HANDLE, set, binding, index);
}

void CCommandContextVk::BindSampler(CSampler& sampler, uint32_t set, uint32_t binding,
                                    uint32_t index)
{
    CurrBindings.BindSampler(static_cast<CSamplerVk&>(sampler).Sampler, set, binding, index);
}

void CCommandContextVk::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    throw "unimplemented";
}

void CCommandContextVk::BindIndexBuffer(CBuffer& buffer, size_t offset, EFormat format)
{
    auto& bufferImpl = static_cast<CBufferVk&>(buffer);
    vkCmdBindIndexBuffer(CmdBuffer->GetHandle(), bufferImpl.Buffer, offset,
                         format == EFormat::R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void CCommandContextVk::BindVertexBuffer(uint32_t binding, CBuffer& buffer, size_t offset)
{
    auto& bufferImpl = static_cast<CBufferVk&>(buffer);
    // On macOS, size_t is 32 bit and VkDeviceSize is 64
    VkDeviceSize offsetDevice = offset;
    vkCmdBindVertexBuffers(CmdBuffer->GetHandle(), binding, 1, &bufferImpl.Buffer, &offsetDevice);
}

void CCommandContextVk::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                             uint32_t firstInstance)
{
    ResolveBindings();
    vkCmdDraw(CmdBuffer->GetHandle(), vertexCount, instanceCount, firstVertex, firstInstance);
}

void CCommandContextVk::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset,
                                    uint32_t firstInstance)
{
    ResolveBindings();
    vkCmdDrawIndexed(CmdBuffer->GetHandle(), indexCount, instanceCount, firstIndex, vertexOffset,
                     firstInstance);
}

void CCommandContextVk::ExecuteCommandList(CCommandList& commandList)
{
    if (IsInRenderPass())
        throw CRHIRuntimeError(
            "CCommandContextVk::ExecuteCommandList can only be called outside of a render pass");

    auto& impl = static_cast<CCommandListVk&>(commandList);
    if (impl.IsRecording())
        throw CRHIRuntimeError("Cannot execute a command list currently being recorded into");

    Flush(false);

    // Create a new command buffer for all the resource transitions
    std::unique_ptr<CCommandBufferVk> preCmdBufferRef;
    preCmdBufferRef = Parent.GetSubmissionTracker()
                          .GetCurrentFrameResources()
                          .FrameCmdPool->AllocateCommandBuffer();
    impl.Submit(Parent.GetSubmissionTracker(), std::move(preCmdBufferRef));
}

void CCommandContextVk::Flush(bool wait, bool isPresent)
{
    EndBuffer();

    // Can't flush a deferred context
    assert(Kind == ECommandContextKind::Immediate || Kind == ECommandContextKind::Transient);

    // Create a new command buffer for all the resource transitions
    std::unique_ptr<CCommandBufferVk> preCmdBufferRef;
    if (Kind == ECommandContextKind::Immediate)
    {
        preCmdBufferRef = Parent.GetSubmissionTracker()
                              .GetCurrentFrameResources()
                              .FrameCmdPool->AllocateCommandBuffer();
    }
    else
    {
        preCmdBufferRef =
            Parent.GetSubmissionTracker().GetTransientPool(QueueType)->AllocateCommandBuffer();
    }
    preCmdBufferRef->BeginRecording(nullptr, 0);
    AccessTracker.DeployAllBarriers(preCmdBufferRef->GetHandle());
    preCmdBufferRef->EndRecording();
    AccessTracker.Clear();

    auto& tracker = Parent.GetSubmissionTracker();
    // Bye bye command buffer, I hope you all well on the GPU side
    std::vector<std::unique_ptr<CCommandBufferVk>> cmdBuffers;
    cmdBuffers.emplace_back(std::move(preCmdBufferRef));
    cmdBuffers.emplace_back(std::move(CmdBuffer));
    CGPUJobInfo job(std::move(cmdBuffers), isPresent, std::move(WaitSemaphores),
                    std::move(WaitStages), { SignalSemaphore }, QueueType,
                    std::move(DeferredDeleters));
    tracker.SubmitJob(std::move(job), wait);

    // Small regression: the signal semaphore might get destroyed before vkQueueSubmit if max frames
    // in flight == 1
    BeginBuffer();
}

void CCommandContextVk::BeginRenderPass(CRenderPass& renderPass,
                                        const std::vector<CClearValue>& clearValues)
{
    if (CurrRenderPass)
        throw CRHIRuntimeError("A render pass is already bound to the context");

    auto& rpImpl = static_cast<CRenderPassVk&>(renderPass);
    auto framebufferInfo = rpImpl.MakeFramebuffer();
    VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    beginInfo.renderPass = rpImpl.GetHandle();
    beginInfo.framebuffer = framebufferInfo.first;
    // If the framebuffer comes with a semaphore, put it into the wait list
    if (framebufferInfo.second != VK_NULL_HANDLE)
    {
        WaitSemaphores.emplace_back(framebufferInfo.second);
        WaitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    std::vector<VkClearValue> clear;
    size_t i = 0;
    for (const auto& c : clearValues)
    {
        if (i >= rpImpl.GetAttachmentDesc().size())
            break;

        VkClearValue vkClear;
        auto aspect = GetImageAspectFlags(rpImpl.GetAttachmentDesc()[i].format);
        if (aspect & VK_IMAGE_ASPECT_COLOR_BIT)
            memcpy(vkClear.color.int32, c.ColorInt32, sizeof(vkClear.color));
        else
        {
            vkClear.depthStencil.depth = c.Depth;
            vkClear.depthStencil.stencil = c.Stencil;
        }
        clear.push_back(vkClear);
        i++;
    }
    beginInfo.clearValueCount = static_cast<uint32_t>(clear.size());
    beginInfo.pClearValues = clear.data();
    beginInfo.renderArea = rpImpl.GetArea();

    vkCmdBeginRenderPass(CmdBuffer->GetHandle(), &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkDevice device = Parent.GetVkDevice();
    VkFramebuffer fb = framebufferInfo.first;
    DeferredDeleters.emplace_back([device, fb]() { vkDestroyFramebuffer(device, fb, nullptr); });

    rpImpl.UpdateImageInitialAccess(AccessTracker);
    CurrRenderPass = &rpImpl;
}

void CCommandContextVk::NextSubpass() { throw "unimplemented"; }

void CCommandContextVk::EndRenderPass()
{
    vkCmdEndRenderPass(CmdBuffer->GetHandle());
    CurrRenderPass->UpdateImageFinalAccess(AccessTracker);
    CurrRenderPass = nullptr;
}

void CCommandContextVk::ResolveBindings()
{
    if (CurrPipeline)
    {
        std::set<uint32_t> setConflicts;
        const auto& pipelineSets = CurrPipeline->GetBoundSets();
        for (const auto& set : pipelineSets)
        {
            // For given set index, if descriptor set layouts differ, store set index in conflicts
            // map.
            if (BoundDescriptorSetLayouts.find(set) != BoundDescriptorSetLayouts.end())
            {
                if (BoundDescriptorSetLayouts.at(set) != CurrPipeline->GetSetLayout(set))
                    setConflicts.emplace(set);
            }
        }

        // Remove entries from boundDescriptorSetLayouts that do not exist in pipeline.
        auto it = BoundDescriptorSetLayouts.begin();
        while (it != BoundDescriptorSetLayouts.end())
        {
            if (!CurrPipeline->GetSetLayout(it->first))
                it = BoundDescriptorSetLayouts.erase(it);
            else
                ++it;
        }

        // Evaluate whether a new descriptor set must be created and bound.
        if (CurrBindings.IsDirty() || !setConflicts.empty())
        {
            // Reset resource bindings dirty bit.
            CurrBindings.ClearDirtyBit();

            // Iterate over each set binding.
            for (auto& setBindingsItr : CurrBindings.GetSetBindings())
            {
                // Skip if no bindings having changes.
                auto set = setBindingsItr.first;
                auto& setBindings = setBindingsItr.second;
                if (!setBindings.bDirty && (setConflicts.find(set) == setConflicts.end()))
                    continue;

                // Reset set binding dirty flag.
                setBindings.bDirty = false;

                // Retrieve the descriptor set layout for the given set index.
                // If set index is not used with bound pipeline, skip it.
                auto descriptorSetLayout = CurrPipeline->GetSetLayout(set);
                if (!descriptorSetLayout)
                    continue;

                // Allocate a new descriptor set. (TODO! Log or report error if allocation fails)
                auto descriptorSet = descriptorSetLayout->AllocateDescriptorSet();
                if (!descriptorSet)
                    continue;

                // Set descriptor set layout as active for given set index.
                BoundDescriptorSetLayouts[set] = descriptorSetLayout;

                // Update all of the set's bindings.
                std::vector<VkDescriptorBufferInfo> bufferInfos;
                std::vector<VkDescriptorImageInfo> imageInfos;
                std::vector<VkWriteDescriptorSet> descriptorWrites;
                for (const auto& bindingItr : setBindings.Bindings)
                {
                    // Get layout binding for given binding index.
                    auto binding = bindingItr.first;
                    VkDescriptorSetLayoutBinding* layoutBinding = nullptr;
                    if (!descriptorSetLayout->GetLayoutBinding(binding, &layoutBinding))
                        continue;

                    // Determine binding's access and pipeline stage flags.
                    auto shaderStages = static_cast<VkShaderStageFlags>(layoutBinding->stageFlags);

                    VkPipelineStageFlags stageMask = 0;
                    if (shaderStages & VK_SHADER_STAGE_VERTEX_BIT)
                        stageMask |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
                    if (shaderStages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
                        stageMask |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT;
                    if (shaderStages & VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
                        stageMask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
                    if (shaderStages & VK_SHADER_STAGE_GEOMETRY_BIT)
                        stageMask |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
                    if (shaderStages & VK_SHADER_STAGE_FRAGMENT_BIT)
                        stageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                    if (shaderStages & VK_SHADER_STAGE_COMPUTE_BIT)
                        stageMask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

                    // Iterate over all array elements.
                    auto& arrayElementBindings = bindingItr.second;
                    for (const auto& arrayElementItr : arrayElementBindings)
                    {
                        // Get the binding info.
                        auto arrayElement = arrayElementItr.first;
                        auto& bindingInfo = arrayElementItr.second;

                        // Fill in descriptor set write structure.
                        VkWriteDescriptorSet dsWrite = {};
                        dsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                        dsWrite.dstSet = descriptorSet;
                        dsWrite.dstBinding = binding;
                        dsWrite.dstArrayElement = arrayElement;
                        dsWrite.descriptorType = layoutBinding->descriptorType;
                        dsWrite.descriptorCount = 1;

                        // Handle buffers.
                        if (bindingInfo.BufferHandle)
                        {
                            VkDescriptorBufferInfo bufferInfo = {};
                            bufferInfo.buffer = bindingInfo.BufferHandle;
                            bufferInfo.offset = bindingInfo.offset;
                            bufferInfo.range = bindingInfo.range;
                            bufferInfos.push_back(bufferInfo);
                            dsWrite.pBufferInfo =
                                reinterpret_cast<const VkDescriptorBufferInfo*>(bufferInfos.size());
                        }
                        // Handle images and samplers.
                        else if (bindingInfo.pImageView || bindingInfo.sampler != VK_NULL_HANDLE)
                        {
                            VkDescriptorImageInfo imageInfo = {};

                            if (bindingInfo.sampler != VK_NULL_HANDLE)
                            {
                                imageInfo.sampler =
                                    reinterpret_cast<VkSampler>(bindingInfo.sampler);
                            }

                            if (bindingInfo.pImageView)
                            {
                                imageInfo.imageView = bindingInfo.pImageView->GetVkImageView();
                                imageInfo.imageLayout =
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // TODO: fixme
                                if (GetImageAspectFlags(bindingInfo.pImageView->GetFormat())
                                    != VK_IMAGE_ASPECT_COLOR_BIT)
                                    imageInfo.imageLayout =
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

                                auto image = bindingInfo.pImageView->GetImage();
                                AccessTracker.TransitionImage(
                                    CmdBuffer->GetHandle(), image.get(),
                                    bindingInfo.pImageView->GetResourceRange(),
                                    VK_ACCESS_SHADER_READ_BIT, stageMask, imageInfo.imageLayout);
                            }

                            imageInfos.push_back(imageInfo);
                            dsWrite.pImageInfo =
                                reinterpret_cast<const VkDescriptorImageInfo*>(imageInfos.size());
                        }

                        // Add the descriptor set write to the list.
                        descriptorWrites.push_back(dsWrite);
                    }
                }

                // Iterate back over descriptorWrites array and fix pointer addresses.
                for (auto& dswrite : descriptorWrites)
                {
                    if (dswrite.pBufferInfo)
                        dswrite.pBufferInfo =
                            &bufferInfos[reinterpret_cast<uint64_t>(dswrite.pBufferInfo) - 1];
                    else if (dswrite.pImageInfo)
                        dswrite.pImageInfo =
                            &imageInfos[reinterpret_cast<uint64_t>(dswrite.pImageInfo) - 1];
                }

                // Update descriptor set.
                vkUpdateDescriptorSets(Parent.GetVkDevice(),
                                       static_cast<uint32_t>(descriptorWrites.size()),
                                       descriptorWrites.data(), 0, nullptr);

                // Store descriptor set binding for current stream encoder position.
                vkCmdBindDescriptorSets(CmdBuffer->GetHandle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        CurrPipeline->GetPipelineLayout(), set, 1, &descriptorSet,
                                        0, nullptr);

                // Add descriptor set to transient resource list.
                DeferredDeleters.push_back([descriptorSetLayout, descriptorSet]() -> void {
                    descriptorSetLayout->FreeDescriptorSet(descriptorSet);
                });
            }
        }
    }
}

}
