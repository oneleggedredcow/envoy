#include "common/upstream/thread_aware_lb_impl.h"

#include <memory>

namespace Envoy {
namespace Upstream {

void ThreadAwareLoadBalancerBase::initialize() {
  // TODO(mattklein123): In the future, once initialized and the initial LB is built, it would be
  // better to use a background thread for computing LB updates. This has the substantial benefit
  // that if the LB computation thread falls behind, host set updates can be trivially collapsed.
  // I will look into doing this in a follow up. Doing everything using a background thread heavily
  // complicated initialization as the load balancer would need its own initialized callback. I
  // think the synchronous/asynchronous split is probably the best option.
  priority_set_.addMemberUpdateCb(
      [this](uint32_t, const HostVector&, const HostVector&) -> void { refresh(); });

  refresh();
}

void ThreadAwareLoadBalancerBase::refresh() {
  auto per_priority_state_vector = std::make_shared<std::vector<PerPriorityStatePtr>>(
      priority_set_.hostSetsPerPriority().size());
  auto healthy_per_priority_load = std::make_shared<HealthyLoad>(healthy_per_priority_load_);
  auto degraded_per_priority_load = std::make_shared<DegradedLoad>(degraded_per_priority_load_);

  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    const uint32_t priority = host_set->priority();
    (*per_priority_state_vector)[priority] = std::make_unique<PerPriorityState>();
    const auto& per_priority_state = (*per_priority_state_vector)[priority];
    // Copy panic flag from LoadBalancerBase. It is calculated when there is a change
    // in hosts set or hosts' health.
    per_priority_state->global_panic_ = per_priority_panic_[priority];
    per_priority_state->current_lb_ =
        createLoadBalancer(*host_set, per_priority_state->global_panic_);
  }

  {
    absl::WriterMutexLock lock(&factory_->mutex_);
    factory_->healthy_per_priority_load_ = healthy_per_priority_load;
    factory_->degraded_per_priority_load_ = degraded_per_priority_load;
    factory_->per_priority_state_ = per_priority_state_vector;
  }
}

HostConstSharedPtr
ThreadAwareLoadBalancerBase::LoadBalancerImpl::chooseHost(LoadBalancerContext* context) {
  // Make sure we correctly return nullptr for any early chooseHost() calls.
  if (per_priority_state_ == nullptr) {
    return nullptr;
  }

  // If there is no hash in the context, just choose a random value (this effectively becomes
  // the random LB but it won't crash if someone configures it this way).
  // computeHashKey() may be computed on demand, so get it only once.
  absl::optional<uint64_t> hash;
  if (context) {
    hash = context->computeHashKey();
  }
  const uint64_t h = hash ? hash.value() : random_.random();

  const uint32_t priority =
      LoadBalancerBase::choosePriority(h, *healthy_per_priority_load_, *degraded_per_priority_load_)
          .first;
  const auto& per_priority_state = (*per_priority_state_)[priority];
  if (per_priority_state->global_panic_) {
    stats_.lb_healthy_panic_.inc();
  }
  return per_priority_state->current_lb_->chooseHost(h);
}

LoadBalancerPtr ThreadAwareLoadBalancerBase::LoadBalancerFactoryImpl::create() {
  auto lb = std::make_unique<LoadBalancerImpl>(stats_, random_);

  // We must protect current_lb_ via a RW lock since it is accessed and written to by multiple
  // threads. All complex processing has already been precalculated however.
  absl::ReaderMutexLock lock(&mutex_);
  lb->healthy_per_priority_load_ = healthy_per_priority_load_;
  lb->degraded_per_priority_load_ = degraded_per_priority_load_;
  lb->per_priority_state_ = per_priority_state_;

  return std::move(lb);
}

} // namespace Upstream
} // namespace Envoy
