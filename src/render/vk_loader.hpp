// Runtime Vulkan loading. The executable never links against libvulkan, so it
// starts on machines without any Vulkan installed (and then uses the CPU
// renderer). Sources of Vulkan, in order:
//   1. MoltenVK linked statically into the binary (macOS release builds,
//      SF_STATIC_MOLTENVK) - Vulkan "baked in", nothing to install.
//   2. The system Vulkan loader, opened with dlopen/LoadLibrary
//      (vulkan-1.dll on Windows, libvulkan.so.1 on Linux - both ship with GPU drivers).
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <string>

namespace sf {

#define SF_VK_GLOBAL_FUNCTIONS(X)            \
    X(vkCreateInstance)                      \
    X(vkEnumerateInstanceExtensionProperties)

#define SF_VK_INSTANCE_FUNCTIONS(X)               \
    X(vkAllocateCommandBuffers)                   \
    X(vkAllocateMemory)                           \
    X(vkBeginCommandBuffer)                       \
    X(vkBindBufferMemory)                         \
    X(vkBindImageMemory)                          \
    X(vkCmdBeginRenderPass)                       \
    X(vkCmdBindPipeline)                          \
    X(vkCmdBindVertexBuffers)                     \
    X(vkCmdCopyImageToBuffer)                     \
    X(vkCmdDraw)                                  \
    X(vkCmdEndRenderPass)                         \
    X(vkCmdPipelineBarrier)                       \
    X(vkCmdPushConstants)                         \
    X(vkCmdSetScissor)                            \
    X(vkCmdSetViewport)                           \
    X(vkCreateBuffer)                             \
    X(vkCreateCommandPool)                        \
    X(vkCreateDevice)                             \
    X(vkCreateFence)                              \
    X(vkCreateFramebuffer)                        \
    X(vkCreateGraphicsPipelines)                  \
    X(vkCreateImage)                              \
    X(vkCreateImageView)                          \
    X(vkCreatePipelineLayout)                     \
    X(vkCreateRenderPass)                         \
    X(vkCreateShaderModule)                       \
    X(vkDestroyBuffer)                            \
    X(vkDestroyCommandPool)                       \
    X(vkDestroyDevice)                            \
    X(vkDestroyFence)                             \
    X(vkDestroyFramebuffer)                       \
    X(vkDestroyImage)                             \
    X(vkDestroyImageView)                         \
    X(vkDestroyInstance)                          \
    X(vkDestroyPipeline)                          \
    X(vkDestroyPipelineLayout)                    \
    X(vkDestroyRenderPass)                        \
    X(vkDestroyShaderModule)                      \
    X(vkDeviceWaitIdle)                           \
    X(vkEndCommandBuffer)                         \
    X(vkEnumerateDeviceExtensionProperties)       \
    X(vkEnumeratePhysicalDevices)                 \
    X(vkFlushMappedMemoryRanges)                  \
    X(vkFreeMemory)                               \
    X(vkGetBufferMemoryRequirements)              \
    X(vkGetDeviceQueue)                           \
    X(vkGetImageMemoryRequirements)               \
    X(vkGetPhysicalDeviceMemoryProperties)        \
    X(vkGetPhysicalDeviceProperties)              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkInvalidateMappedMemoryRanges)             \
    X(vkMapMemory)                                \
    X(vkQueueSubmit)                              \
    X(vkResetCommandBuffer)                       \
    X(vkResetFences)                              \
    X(vkUnmapMemory)                              \
    X(vkWaitForFences)

#define SF_VK_DECLARE(name) extern PFN_##name name;
SF_VK_DECLARE(vkGetInstanceProcAddr)
SF_VK_GLOBAL_FUNCTIONS(SF_VK_DECLARE)
SF_VK_INSTANCE_FUNCTIONS(SF_VK_DECLARE)
#undef SF_VK_DECLARE

// Finds a Vulkan implementation and loads global entry points. Returns false
// (with a reason) when none is available. Safe to call repeatedly.
bool loadVulkan(std::string& error, std::string& source);
// Loads every instance/device-level function used by the renderer.
bool loadVulkanInstanceFunctions(VkInstance instance, std::string& error);

}  // namespace sf
