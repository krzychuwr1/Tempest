#if defined(TEMPEST_BUILD_VULKAN)

#include "vfence.h"
#include "vdevice.h"

using namespace Tempest::Detail;

void VFence::wait() {
  vkAssert(device->waitFence(*this, std::numeric_limits<uint64_t>::max()));
  }

bool VFence::wait(uint64_t time) {
  VkResult res = device->waitFence(*this, time);
  if(res==VK_NOT_READY)
    return false;
  vkAssert(res);
  return true;
  }

void VFence::setStatus(VkResult st) {
  status = st;
  if(st < 0 || st==VK_SUCCESS) {
    holdRes.clear();
    }
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
