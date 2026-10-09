/* SwanStationPS5 - the Vulkan commands frame generation uses (third_party/framegen),
 * as pointers SwanStationPS5 fills in (fg_bridge.cpp beside it): the app has no Vulkan loader.
 * Included before everything else when tools/build-framegen.sh compiles it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define FG_VK_INSTANCE_FUNCS(X)                                                                    \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) X(vkGetDeviceProcAddr)

#define FG_VK_DEVICE_FUNCS(X)                                                                      \
    X(vkAllocateDescriptorSets) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkBindImageMemory)    \
    X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdClearColorImage) X(vkCmdDispatch)       \
    X(vkCmdFillBuffer) X(vkCmdPipelineBarrier) X(vkCmdResetQueryPool) X(vkCreateBuffer)            \
    X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout)          \
    X(vkCreateImage) X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateQueryPool)          \
    X(vkCreateSampler) X(vkCreateShaderModule) X(vkDestroyBuffer) X(vkDestroyDescriptorPool)      \
    X(vkDestroyDescriptorSetLayout) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyPipeline)  \
    X(vkDestroyPipelineLayout) X(vkDestroyQueryPool) X(vkDestroySampler) X(vkDestroyShaderModule) \
    X(vkFreeMemory) X(vkGetBufferMemoryRequirements) X(vkGetImageMemoryRequirements)               \
    X(vkGetQueryPoolResults) X(vkMapMemory) X(vkUnmapMemory) X(vkUpdateDescriptorSets)            \
    X(vkCmdWriteTimestamp)

#define FG_VK_DECLARE(name) extern PFN_##name name;
FG_VK_INSTANCE_FUNCS(FG_VK_DECLARE)
FG_VK_DEVICE_FUNCS(FG_VK_DECLARE)
#undef FG_VK_DECLARE
