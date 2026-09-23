// Exclusive scopes, leases and durable fencing tokens.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_AUTHORITY_HPP
#define SNCF_AUTHORITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sncf/ids.hpp"

namespace sncf {

/// The unit of exclusivity. A scope is either a single port of a device or the
/// whole device when the function is not port-scoped. Two live instances can
/// never hold the same scope; the fabric proves this by construction.
struct ExclusiveScope {
  SmartNicId smartnic;
  DeviceId device;
  /// Nil means the scope covers the whole device rather than one port.
  PortId port;

  [[nodiscard]] bool valid() const noexcept { return smartnic.valid() && device.valid(); }
  [[nodiscard]] bool is_port_scope() const noexcept { return port.valid(); }
  /// Canonical, stable, parseable key used for ordering, persistence and export.
  [[nodiscard]] std::string to_key() const;

  friend bool operator==(const ExclusiveScope& a, const ExclusiveScope& b) noexcept {
    return a.smartnic == b.smartnic && a.device == b.device && a.port == b.port;
  }
  friend bool operator!=(const ExclusiveScope& a, const ExclusiveScope& b) noexcept { return !(a == b); }
  friend bool operator<(const ExclusiveScope& a, const ExclusiveScope& b) noexcept {
    if (a.smartnic != b.smartnic) {
      return a.smartnic < b.smartnic;
    }
    if (a.device != b.device) {
      return a.device < b.device;
    }
    return a.port < b.port;
  }
};

/// A lease is the control-plane right to act on a scope. It is not an effect.
struct Lease {
  LeaseId id;
  ExclusiveScope scope;
  FunctionInstanceId instance;
  PrincipalId holder;
  CoordinatorEpoch epoch;
  FencingToken token;
  TimestampNs granted_at;
  TimestampNs expires_at;
  bool revoked = false;

  /// A lease is live only when it is unrevoked, unexpired and was issued by the
  /// current coordinator epoch. Every term is explicit; nothing is inferred.
  [[nodiscard]] bool is_live(TimestampNs now, CoordinatorEpoch live_epoch) const noexcept {
    return !revoked && epoch == live_epoch && now.value() < expires_at.value();
  }
};

/// The authority a caller must present for a mutating operation. Absent
/// authority is never treated as coordinator authority.
struct AuthorityClaim {
  ExclusiveScope scope;
  FunctionInstanceId instance;
  LeaseId lease;
  FencingToken token;
  CoordinatorEpoch epoch;
  PrincipalId principal;
};

struct AuthorityDecision {
  bool granted = false;
  ReasonCode reason = ReasonCode::RefusedNoAuthority;
  LeaseId lease;
  FencingToken token;
  CoordinatorEpoch epoch;
  TimestampNs expires_at;
};

/// Durable fencing high-water marks. Tokens are strictly monotonic per scope and
/// survive restart: a token issued before a restart can never be accepted after
/// it, which is what fences stale incarnations.
class FencingLedger {
 public:
  void observe(const ExclusiveScope& scope, FencingToken token) noexcept;
  [[nodiscard]] FencingToken high_water(const ExclusiveScope& scope) const noexcept;
  /// Allocates the next token for a scope, or std::nullopt on exhaustion.
  [[nodiscard]] std::optional<FencingToken> allocate(const ExclusiveScope& scope) noexcept;
  /// Advances the high-water mark without allocating a lease (used at recovery
  /// so that every pre-restart token is fenced).
  [[nodiscard]] std::optional<FencingToken> advance(const ExclusiveScope& scope) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<std::pair<ExclusiveScope, FencingToken>>& entries() const noexcept {
    return entries_;
  }
  void clear() noexcept { entries_.clear(); }

 private:
  std::vector<std::pair<ExclusiveScope, FencingToken>> entries_;  // kept sorted by scope
};

}  // namespace sncf

#endif  // SNCF_AUTHORITY_HPP
