#include "CommandContextVk.h"
#include "BufferVk.h"
#include "CommandQueueVk.h"
#include "DescriptorSetVk.h"
#include "DeviceVk.h"
#include "ImageViewVk.h"
#include "ImageVk.h"
#include "PipelineVk.h"
#include "RenderPassVk.h"

namespace RHI
{

CRenderPassContextVk::CRenderPassContextVk(CCommandListVk::Ref cmdList, CRenderPass::Ref renderPass,
                                           std::vector<CClearValue> clearValues)
    : CmdList(std::move(cmdList))
    , RenderPass(std::move(renderPass))
    , ClearValues(std::move(clearValues))
{
    if (CmdList->IsCommitted())
        throw CRHIRuntimeError("A committed command list can no longer be recorded into");
    if (CmdList->bIsContextActive)
        throw CRHIRuntimeError("One context is already active on this command list");
    CmdList->bIsContextActive = true;

    auto rpImpl = std::static_pointer_cast<CRenderPassVk>(RenderPass);
    SubpassInfos.resize(rpImpl->GetSubpassCount());
}

CRenderPassContextVk::~CRenderPassContextVk()
{
    if (CmdList && CmdList->bIsContextActive)
    {
        throw CRHIRuntimeError("Command Context destroyed before FinishRecording");
    }
}

uint32_t CRenderPassContextVk::MakeSubpassInfo(uint32_t subpass)
{
    std::lock_guard<tc::FSpinLock> lk(SpinLock);
    uint32_t ret = static_cast<uint32_t>(SubpassInfos[subpass].size());
    SubpassInfos[subpass].emplace_back();
    return ret;
}

IRenderContext::Ref CRenderPassContextVk::CreateRenderContext(uint32_t subpass)
{
    return std::make_shared<CCommandContextVk>(shared_from_this(), subpass);
}

void CRenderPassContextVk::FinishRecording()
{
    static_assert(sizeof(VkClearValue) == sizeof(CClearValue), "Struct sizes mismatch");
    // TODO: make sure all those render contexts are done

    CCommandListSection section;
    auto& allocator = CmdList->GetQueue().GetCmdBufferAllocator();
    section.CmdBuffer = allocator.Allocate(false);
    // Record all those render lists
    {
        section.CmdBuffer->BeginRecording(VK_NULL_HANDLE, 0);
        VkCommandBuffer handle = section.CmdBuffer->GetHandle();

        auto renderPass = std::static_pointer_cast<CRenderPassVk>(RenderPass);
        std::vector<VkSemaphore> waitSemaphores;
        auto framebuffer = renderPass->MakeFramebuffer(waitSemaphores, section.SignalSemaphores);
        for (VkSemaphore semaphore : waitSemaphores)
        {
            section.WaitSemaphores.push_back(semaphore);
            section.WaitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            CmdList->GetQueue().GetDevice().AddPostFrameCleanup([semaphore](CDeviceVk& p) {
                vkDestroySemaphore(p.GetVkDevice(), semaphore, nullptr);
            });
        }

        VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        beginInfo.renderPass = renderPass->GetHandle();
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea = renderPass->GetArea();
        beginInfo.clearValueCount = ClearValues.size();
        beginInfo.pClearValues = reinterpret_cast<VkClearValue*>(ClearValues.data());
        vkCmdBeginRenderPass(handle, &beginInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        for (uint32_t i = 0; i < renderPass->GetSubpassCount(); i++)
        {
            std::vector<VkCommandBuffer> secondaryBuffers;
            for (auto& subpassInfo : SubpassInfos[i])
            {
                auto& bufferRef = subpassInfo.SecondaryBuffer;
                secondaryBuffers.emplace_back(bufferRef->GetHandle());
                section.SecondaryBuffers.emplace_back(std::move(bufferRef));

                section.AccessTracker.Merge(VK_NULL_HANDLE, subpassInfo.AccessTracker);
            }

            vkCmdExecuteCommands(handle, secondaryBuffers.size(), secondaryBuffers.data());

            if (i == renderPass->GetSubpassCount() - 1)
                vkCmdEndRenderPass(handle);
            else
                vkCmdNextSubpass(handle, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        }
        section.CmdBuffer->EndRecording();
    }
    CmdList->Sections.emplace_back(std::move(section));

    // Grab a new command buffer to write all the resource transitions
    if (CmdList->Sections.size() > 1)
    {
        CmdList->Sections.back().PreCmdBuffer =
            CmdList->GetQueue().GetCmdBufferAllocator().Allocate();
        CmdList->Sections.back().PreCmdBuffer->BeginRecording(VK_NULL_HANDLE, 0);
        CmdList->Sections[0].AccessTracker.Merge(CmdList->Sections.back().PreCmdBuffer->GetHandle(),
                                                 CmdList->Sections.back().AccessTracker);
        CmdList->Sections.back().PreCmdBuffer->EndRecording();
        CmdList->Sections.back().AccessTracker.Clear();
    }

    // Drop reference
    CmdList->bIsContextActive = false;
    CmdList.reset();
}

void CCommandContextVk::Convert(VkOffset2D& dst, const COffset2D& src)
{
    static_assert(sizeof(VkOffset2D) == sizeof(COffset2D), "struct size mismatch");
    dst = reinterpret_cast<const VkOffset2D&>(src);
}

void CCommandContextVk::Convert(VkExtent2D& dst, const CExtent2D& src)
{
    static_assert(sizeof(VkExtent2D) == sizeof(CExtent2D), "struct size mismatch");
    dst = reinterpret_cast<const VkExtent2D&>(src);
}

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

void CCommandContextVk::Convert(VkViewport& dst, const CViewportDesc& src)
{
    dst.x = src.X;
    dst.y = src.Y;
    dst.width = src.Width;
    dst.height = src.Height;
    dst.minDepth = src.MinDepth;
    dst.maxDepth = src.MaxDepth;
}

void CCommandContextVk::Convert(VkRect2D& dst, const CRect2D& src)
{
    Convert(dst.offset, src.Offset);
    Convert(dst.extent, src.Extent);
}

CCommandContextVk::CCommandContextVk(const CCommandListVk::Ref& cmdList)
    : CmdList(cmdList)
{
    if (CmdList->IsCommitted())
        throw CRHIRuntimeError("A committed command list can no longer be recorded into");
    if (CmdList->bIsContextActive)
        throw CRHIRuntimeError("One context is already active on this command list");
    CmdList->bIsContextActive = true;

    CCommandListSection section;
    auto& allocator = cmdList->GetQueue().GetCmdBufferAllocator();
    section.CmdBuffer = allocator.Allocate(false);
    section.CmdBuffer->BeginRecording(nullptr, 0);
    CmdList->Sections.emplace_back(std::move(section));
}

CCommandContextVk::CCommandContextVk(const CRenderPassContextVk::Ref& renderPassContext,
                                     uint32_t subpass)
{
    RenderPassContext = renderPassContext;
    SubpassIndex = subpass;
    CmdBufferIndex = renderPassContext->MakeSubpassInfo(subpass);

    auto& allocator = renderPassContext->GetCmdList()->GetQueue().GetCmdBufferAllocator();
    auto cmdBuffer = allocator.Allocate(true);
    cmdBuffer->BeginRecording(renderPassContext->GetRenderPass(), subpass);

    auto& subpassInfo = renderPassContext->GetSubpassInfo(subpass, CmdBufferIndex);
    subpassInfo.SecondaryBuffer = std::move(cmdBuffer);

    auto rpImpl = std::static_pointer_cast<CRenderPassVk>(RenderPassContext->GetRenderPass());
    CViewportDesc vp {};
    vp.X = 0;
    vp.Y = 0;
    vp.Width = rpImpl->GetArea().extent.width;
    vp.Height = rpImpl->GetArea().extent.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    SetViewport(vp);

    CRect2D scissor {};
    scissor.Offset.Set(0, 0);
    scissor.Extent.Set(vp.Width, vp.Height);
    SetScissor(scissor);
}

CCommandContextVk::~CCommandContextVk()
{
    if (CmdList && CmdList->bIsContextActive)
    {
        throw CRHIRuntimeError("Command Context destroyed before FinishRecording");
    }
}

void CCommandContextVk::TransitionImage(CImage& image, EResourceState newState)
{
    auto& imageImpl = static_cast<CImageVk&>(image);
    CImageSubresourceRange range;
    range.BaseArrayLayer = 0;
    range.BaseMipLevel = 0;
    range.LayerCount = imageImpl.GetArrayLayers();
    range.LevelCount = imageImpl.GetMipLevels();
    AccessTracker().TransitionImageState(CmdBuffer(), &imageImpl, range, newState,
                                         CmdList->GetQueue().GetType() == EQueueType::Copy);
}

void CCommandContextVk::CopyBuffer(CBuffer& src, CBuffer& dst,
                                   const std::vector<CBufferCopy>& regions)
{
    static_assert(sizeof(CBufferCopy) == sizeof(VkBufferCopy), "struct size mismatch");
    const auto* r = reinterpret_cast<const VkBufferCopy*>(regions.data());

    vkCmdCopyBuffer(CmdBuffer(), static_cast<CBufferVk&>(src).Buffer,
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
    vkCmdCopyImage(CmdBuffer(), srcImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   static_cast<uint32_t>(r.size()), r.data());
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
    vkCmdCopyBufferToImage(CmdBuffer(), static_cast<CBufferVk&>(src).Buffer, dstImpl.GetVkImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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
    vkCmdCopyImageToBuffer(CmdBuffer(), srcImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
    vkCmdBlitImage(CmdBuffer(), srcImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   static_cast<uint32_t>(r.size()), r.data(), VkCast(filter));
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
    vkCmdResolveImage(CmdBuffer(), srcImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dstImpl.GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      static_cast<uint32_t>(r.size()), r.data());
}

void CCommandContextVk::BindComputePipeline(CPipeline& pipeline)
{
    auto& impl = static_cast<CPipelineVk&>(pipeline);
    CurrPipeline = &impl;
    vkCmdBindPipeline(CmdBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, impl.GetHandle());
}

void CCommandContextVk::BindComputeDescriptorSet(uint32_t set, CDescriptorSet& descriptorSet)
{
    // TODO: access tracking
    auto& impl = static_cast<CDescriptorSetVk&>(descriptorSet);
    VkDescriptorSet setHandle = impl.GetHandle(true);
    impl.WriteUpdates();
    vkCmdBindDescriptorSets(CmdBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE,
                            CurrPipeline->GetPipelineLayout(), set, 1, &setHandle, 0, nullptr);
}

void CCommandContextVk::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    vkCmdDispatch(CmdBuffer(), groupCountX, groupCountY, groupCountZ);
}

void CCommandContextVk::DispatchIndirect(CBuffer& buffer, size_t offset)
{
    auto& impl = static_cast<CBufferVk&>(buffer);
    vkCmdDispatchIndirect(CmdBuffer(), impl.Buffer, offset);
}

void CCommandContextVk::BindRenderPipeline(CPipeline& pipeline)
{
    auto& impl = static_cast<CPipelineVk&>(pipeline);
    CurrPipeline = &impl;
    vkCmdBindPipeline(CmdBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, impl.GetHandle());
}

void CCommandContextVk::SetViewport(const CViewportDesc& viewportDesc)
{
    VkViewport vp;
    Convert(vp, viewportDesc);
    vkCmdSetViewport(CmdBuffer(), 0, 1, &vp);
}

void CCommandContextVk::SetScissor(const CRect2D& scissor)
{
    VkRect2D region;
    Convert(region, scissor);
    vkCmdSetScissor(CmdBuffer(), 0, 1, &region);
}

void CCommandContextVk::SetBlendConstants(const std::array<float, 4>& blendConstants)
{
    vkCmdSetBlendConstants(CmdBuffer(), blendConstants.data());
}

void CCommandContextVk::SetStencilReference(uint32_t reference)
{
    vkCmdSetStencilReference(CmdBuffer(), VK_STENCIL_FRONT_AND_BACK, reference);
}

void CCommandContextVk::BindRenderDescriptorSet(uint32_t set, CDescriptorSet& descriptorSet)
{
    if (!CurrPipeline)
        throw CRHIRuntimeError("Cannot BindRenderDescriptorSet without a bound pipeline");
    // TODO: access tracking
    auto& impl = static_cast<CDescriptorSetVk&>(descriptorSet);
    VkDescriptorSet setHandle = impl.GetHandle(true);
    impl.WriteUpdates();
    vkCmdBindDescriptorSets(CmdBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            CurrPipeline->GetPipelineLayout(), set, 1, &setHandle, 0, nullptr);
}

void CCommandContextVk::BindIndexBuffer(CBuffer& buffer, size_t offset, EFormat format)
{
    auto& impl = static_cast<CBufferVk&>(buffer);
    VkIndexType indexType =
        format == EFormat::R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(CmdBuffer(), impl.Buffer, offset, indexType);
}

void CCommandContextVk::BindVertexBuffer(uint32_t binding, CBuffer& buffer, size_t offset)
{
    auto& impl = static_cast<CBufferVk&>(buffer);
    // Workaround for systems where size_t != 8
    VkDeviceSize vkOffset = offset;
    vkCmdBindVertexBuffers(CmdBuffer(), binding, 1, &impl.Buffer, &vkOffset);
}

void CCommandContextVk::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                             uint32_t firstInstance)
{
    vkCmdDraw(CmdBuffer(), vertexCount, instanceCount, firstVertex, firstInstance);
}

void CCommandContextVk::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset,
                                    uint32_t firstInstance)
{
    vkCmdDrawIndexed(CmdBuffer(), indexCount, instanceCount, firstIndex, vertexOffset,
                     firstInstance);
}

void CCommandContextVk::DrawIndirect(CBuffer& buffer, size_t offset, uint32_t drawCount,
                                     uint32_t stride)
{
    VkBuffer vkBuffer = static_cast<CBufferVk&>(buffer).Buffer;
    vkCmdDrawIndirect(CmdBuffer(), vkBuffer, offset, drawCount, stride);
}

void CCommandContextVk::DrawIndexedIndirect(CBuffer& buffer, size_t offset, uint32_t drawCount,
                                            uint32_t stride)
{
    VkBuffer vkBuffer = static_cast<CBufferVk&>(buffer).Buffer;
    vkCmdDrawIndirect(CmdBuffer(), vkBuffer, offset, drawCount, stride);
}

void CCommandContextVk::FinishRecording()
{
    if (CmdList)
    {
        CmdList->Sections.back().CmdBuffer->EndRecording();

        // Grab a new command buffer to write all the resource transitions
        if (CmdList->Sections.size() > 1)
        {
            CmdList->Sections.back().PreCmdBuffer =
                CmdList->GetQueue().GetCmdBufferAllocator().Allocate();
            CmdList->Sections.back().PreCmdBuffer->BeginRecording(VK_NULL_HANDLE, 0);
            CmdList->Sections[0].AccessTracker.Merge(
                CmdList->Sections.back().PreCmdBuffer->GetHandle(),
                CmdList->Sections.back().AccessTracker);
            CmdList->Sections.back().PreCmdBuffer->EndRecording();
            CmdList->Sections.back().AccessTracker.Clear();
        }

        // Drop reference
        CmdList->bIsContextActive = false;
        CmdList.reset();
    }
    else
    {
        RenderPassContext->GetSubpassInfo(SubpassIndex, CmdBufferIndex)
            .SecondaryBuffer->EndRecording();
        RenderPassContext.reset();
    }
}

CAccessTracker& CCommandContextVk::AccessTracker()
{
    if (CmdList)
        return CmdList->Sections.back().AccessTracker;
    return RenderPassContext->GetSubpassInfo(SubpassIndex, CmdBufferIndex).AccessTracker;
}

VkCommandBuffer CCommandContextVk::CmdBuffer()
{
    if (CmdList)
        return CmdList->Sections.back().CmdBuffer->GetHandle();
    return RenderPassContext->GetSubpassInfo(SubpassIndex, CmdBufferIndex)
        .SecondaryBuffer->GetHandle();
}

}
