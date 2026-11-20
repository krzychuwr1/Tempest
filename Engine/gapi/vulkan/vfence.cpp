#if defined(TEMPEST_BUILD_VULKAN)

#include "vfence.h"
#include "vdevice.h"

#include <chrono>
#include <thread>

using namespace Tempest::Detail;

void VFence::wait() {
  for(;;) {
    const auto status = device->waitFence(*this,std::numeric_limits<uint64_t>::max());
    if(status!=VK_ERROR_OUT_OF_HOST_MEMORY && status!=VK_ERROR_OUT_OF_DEVICE_MEMORY) {
      vkAssert(status);
      return;
      }
    // A blocking wait also protects the command buffer and its staging resources.
    // Retry outside the timeline lock so unrelated completed fences can be recycled.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

bool VFence::wait(uint64_t time) {
  VkResult res = device->waitFence(*this, time);
  if(res==VK_NOT_READY)
    return false;
  vkAssert(res);
  return true;
  }

void VFence::setStatus(VkResult st) {
  if(st!=VK_SUCCESS && st!=VK_ERROR_DEVICE_LOST)
    return; // A failed poll or wait does not prove completion.
  status = st;
  holdRes.clear();
  }

void VFence::setPayload(std::vector<ResPtr>&& res) {
  {
    std::lock_guard<std::mutex> guard(device->timeline.sync);
    if(status==VK_NOT_READY) {
      holdRes = std::move(res);
      return;
      }
    }
  // The submission completed before its staging resources reached the fence.
  res.clear();
  }

void VFence::clearPayload() {
  holdRes.clear();
  }

#endif
