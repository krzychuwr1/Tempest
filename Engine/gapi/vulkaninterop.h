#pragma once

#include "vulkanapi.h"
#include "vulkan/vulkan_sdk.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace Tempest {

class Device;
class Attachment;
class ZBuffer;
class CommandBuffer;
template<class T> class Encoder;
enum TextureFormat : uint8_t;

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

VulkanNativeImage   vulkanNativeImage(const Attachment& image);
// Render target over an external RGBA8-compatible colour image (created with a mutable format,
// such as an sRGB OpenXR swapchain image), viewed as R8G8B8A8_UNORM so values are written as they are
Attachment          vulkanBorrowColorAttachment(Device& device, VkImage image, uint32_t width, uint32_t height);
// Same for other colour formats (for example RGBA16F motion vectors) and for depth images
Attachment          vulkanBorrowColorAttachment(Device& device, VkImage image, uint32_t width, uint32_t height, TextureFormat format);
ZBuffer             vulkanBorrowDepthAttachment(Device& device, VkImage image, uint32_t width, uint32_t height, TextureFormat format);
// Multiview: render targets over external images with several array layers (one per view),
// and the view mask of the render passes begun next (0: ordinary single-view rendering)
Attachment          vulkanBorrowLayeredAttachment(Device& device, VkImage image, uint32_t width, uint32_t height, TextureFormat format, uint32_t layers);
ZBuffer             vulkanBorrowLayeredDepth(Device& device, VkImage image, uint32_t width, uint32_t height, TextureFormat format, uint32_t layers);
void                vulkanSetViewMask(uint32_t mask);

// Upload one byte per rate texel before the next framebuffer in this encoder. The caller
// retains the buffer, image and view until the submitted command buffer has completed.
struct VulkanShadingRateAttachment {
  VkBuffer   source = VK_NULL_HANDLE;
  VkImage    image  = VK_NULL_HANDLE;
  VkImageView view  = VK_NULL_HANDLE;
  VkExtent2D extent = {};
  VkExtent2D texel  = {};
  };
void vulkanSetNextShadingRate(Encoder<CommandBuffer>& enc, const VulkanShadingRateAttachment& rate);

}
