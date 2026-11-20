#include "fence.h"
#include <Tempest/Except>

using namespace Tempest;

Fence::Fence(std::shared_ptr<AbstractGraphicsApi::Fence>& f)
  : impl(f) {
  }

Fence::~Fence() {
  try {
    wait();
    }
  catch(const DeviceLostException&) {
    // Terminal waits retire resource use. Explicit wait() still reports the loss.
    }
  }

void Fence::wait() {
  if(impl != nullptr) {
    impl->wait();
    return;
    }
  }

bool Fence::wait(uint64_t time) {
  if(impl != nullptr) {
    return impl->wait(time);
    }
  return true;
  }
