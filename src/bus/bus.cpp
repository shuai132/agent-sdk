#include "bus/bus.hpp"

namespace agent {

Bus &Bus::instance() {
  static Bus instance;
  return instance;
}

void Bus::unsubscribe(SubscriptionId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &[type_idx, handlers] : handlers_) {
    handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                  [id](const HandlerEntry &entry) {
                                    return entry.id == id;
                                  }),
                   handlers.end());
  }
}

}  // namespace agent
