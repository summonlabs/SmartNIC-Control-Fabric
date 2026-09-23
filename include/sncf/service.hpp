// Protocol-to-runtime adapter. This is the only place where wire documents are
// decoded into typed requests, so malformed input is refused once, in one place,
// with stable reason codes.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_SERVICE_HPP
#define SNCF_SERVICE_HPP

#include <atomic>
#include <string>
#include <string_view>

#include "sncf/canonical.hpp"
#include "sncf/fabric.hpp"

namespace sncf {

class FabricService {
 public:
  explicit FabricService(Fabric& fabric) noexcept : fabric_(fabric) {}

  /// Dispatches one operation. Returns the result document, or a refusal with a
  /// stable reason code. Unknown operations are refused, never ignored.
  [[nodiscard]] Result<Value> dispatch(std::string_view operation, const Value& arguments) noexcept;

  /// Set once a shutdown request has been accepted, so the server can drain.
  [[nodiscard]] bool shutdown_requested() const noexcept {
    return shutdown_requested_.load(std::memory_order_acquire);
  }
  void clear_shutdown() noexcept { shutdown_requested_.store(false, std::memory_order_release); }

  [[nodiscard]] static std::string_view operations_help() noexcept;

 private:
  Fabric& fabric_;
  std::atomic<bool> shutdown_requested_{false};
};

}  // namespace sncf

#endif  // SNCF_SERVICE_HPP
