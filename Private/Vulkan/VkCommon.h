#pragma once
#include <Platform.h>
#include <RHIException.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#if TC_OS == TC_OS_MAC_OS_X
#define VK_USE_PLATFORM_MACOS_MVK
#define __FUNCSIG__ __PRETTY_FUNCTION__
#endif

#if TC_OS == TC_OS_LINUX
#define VK_USE_PLATFORM_LINUX_KHR
#define __FUNCSIG__ __PRETTY_FUNCTION__
#endif

#include "vk_mem_alloc.h"

namespace RHI
{

class CDeviceVk;
class CCommandQueueVk;
class CCommandContextVk;
class CDescriptorSetVk;
class CImageVk;
class CImageViewVk;
class CPipelineVk;

#define VK(fn)                                                                                     \
    do                                                                                             \
    {                                                                                              \
        VkResult err = fn;                                                                         \
        if (err != VK_SUCCESS)                                                                     \
        {                                                                                          \
            printf("%s = %d\n", __FUNCSIG__, err);                                                 \
            throw CRHIRuntimeError("Vulkan call did not return VK_SUCCESS");                       \
        }                                                                                          \
    } while (0)

}
