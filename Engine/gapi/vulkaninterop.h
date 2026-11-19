#pragma once

#include "vulkan/vulkan_sdk.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace Tempest {

class Device;
class Attachment;

// Lets an external runtime (such as OpenXR) create the Vulkan instance and device.
// Install the hooks before constructing VulkanApi; Tempest still fills in the
// extensions and features it needs, and the hooks may add their own.
struct VulkanCreateHooks {
  void*            context = nullptr;
  VkResult         (*createInstance)(void* context, const VkInstanceCreateInfo* info, VkInstance* out) = nullptr;
  VkPhysicalDevice (*physicalDevice)(void* context, VkInstance instance) = nullptr;
  VkResult         (*createDevice)(void* context, VkPhysicalDevice pdev, const VkDeviceCreateInfo* info, VkDevice* out) = nullptr;
  };

VulkanCreateHooks& vulkanCreateHooks();

struct VulkanNativeContext {
  VkInstance       instance       = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice         device         = VK_NULL_HANDLE;
  VkQueue          queue          = VK_NULL_HANDLE;
  uint32_t         queueFamily    = 0;
  // Tempest submits under this lock; external submissions to the same queue must take it too
  std::mutex*      queueLock      = nullptr;
  };

struct VulkanNativeImage {
  VkImage       image         = VK_NULL_HANDLE;
  VkFormat      format        = VK_FORMAT_UNDEFINED;
  // Layout Tempest leaves the image in between command buffers
  VkImageLayout restingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  };

VulkanNativeContext vulkanNativeContext(Device& device);

// GPU time per debug-marker region, measured with timestamp queries while enabled.
// The report lists average milliseconds per command buffer since the previous report.
void        vulkanSetGpuProfiling(bool enabled);
bool        vulkanGpuProfiling();
std::string vulkanGpuProfileReport();
VulkanNativeImage   vulkanNativeImage(const Attachment& image);
// Render target over an external RGBA8-compatible colour image (created with a mutable format,
// such as an sRGB OpenXR swapchain image), viewed as R8G8B8A8_UNORM so values are written as they are
Attachment          vulkanBorrowColorAttachment(Device& device, VkImage image, uint32_t width, uint32_t height);

}
