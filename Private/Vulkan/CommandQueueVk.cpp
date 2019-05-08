#include "CommandQueueVk.h"
#include "CommandListVk.h"
#include "DeviceVk.h"

namespace RHI
{

CCommandQueueVk::CCommandQueueVk(CDeviceVk& p, EQueueType queueType, VkQueue handle)
    : Parent(p), Type(queueType), Handle(handle), CmdBufferAllocator(p, queueType), FrameResources{p, p, p}
{
    // Fences are created signaled, unsignal the first one
    vkWaitForFences(Parent.GetVkDevice(), 1, &FrameResources[CurrFrameIndex].Fence, VK_TRUE, 1000);
}

CCommandQueueVk::~CCommandQueueVk()
{
    Finish();
}

CCommandList::Ref CCommandQueueVk::CreateCommandList()
{
    return std::make_shared<CCommandListVk>(*this);
}

void CCommandQueueVk::Flush()
{
    Submit();
}

void CCommandQueueVk::Finish()
{
    Flush();
    vkQueueWaitIdle(GetHandle());
}

void CCommandQueueVk::EnqueueCommandList(CCommandListVk::Ref cmdList)
{
    std::lock_guard<std::mutex> lk(Mutex);
    QueuedLists.push_back(std::move(cmdList));
}

void CCommandQueueVk::Submit(bool setFence)
{
    std::lock_guard<std::mutex> lk(Mutex);

    // Try to submit all queued lists that are committed
    std::vector<VkCommandBuffer> cmdBufferStaging;
    cmdBufferStaging.reserve(512);
    std::vector<VkSubmitInfo> submitInfos;
    size_t submittedCount = 0;
    for (const auto& list : QueuedLists)
    {
        bool ready = list->IsCommitted();
        if (ready)
        {
            list->MakeSubmitInfos(submitInfos, cmdBufferStaging);

            // Move this list to the in flight list
            FrameResources[CurrFrameIndex].ListsInFlight.emplace_back(list);
            submittedCount++;
        }
    }
    QueuedLists.erase(QueuedLists.begin(), QueuedLists.begin() + submittedCount);
    if (cmdBufferStaging.size() > 512)
        throw CRHIException("Umm, tell Toby about this");
    if (setFence)
        VK(vkQueueSubmit(GetHandle(), submitInfos.size(), submitInfos.data(), FrameResources[CurrFrameIndex].Fence));
    else
        VK(vkQueueSubmit(GetHandle(), submitInfos.size(), submitInfos.data(), VK_NULL_HANDLE));
    assert(FrameResources[CurrFrameIndex].PostFrameCleanup.empty());

    std::lock_guard<std::mutex> lkd(GetDevice().DeviceMutex);
    FrameResources[CurrFrameIndex].PostFrameCleanup = std::move(GetDevice().PostFrameCleanup);
}

void CCommandQueueVk::SubmitFrame()
{
    // Do Submit() and advance frame index
    Submit(true);

    // Advance
    CurrFrameIndex++;
    CurrFrameIndex %= FrameIndexCount;
    VK(vkWaitForFences(Parent.GetVkDevice(), 1, &FrameResources[CurrFrameIndex].Fence, VK_TRUE, 1000000));
    FrameResources[CurrFrameIndex].Reset();
}

CCommandQueueVk::CFrameResources::CFrameResources(CDeviceVk& deviceVk)
    : DeviceVk(deviceVk)
{
    // We create the fences signaled so that vkWaitForFences returns immediately
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK(vkCreateFence(DeviceVk.GetVkDevice(), &fenceInfo, nullptr, &Fence));
}

CCommandQueueVk::CFrameResources::~CFrameResources()
{
    vkDestroyFence(DeviceVk.GetVkDevice(), Fence, nullptr);
}

void CCommandQueueVk::CFrameResources::Reset()
{
    for (const auto& ptr : ListsInFlight)
        ptr->ReleaseAllResources();
    for (const auto& cleanupFn : PostFrameCleanup)
        cleanupFn(DeviceVk);
    ListsInFlight.clear();
    PostFrameCleanup.clear();
}

}
