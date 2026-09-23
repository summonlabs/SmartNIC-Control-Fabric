// Copyright 2026 Summon Software Labs.
#include "sncf/authority.hpp"

#include <algorithm>
#include <limits>

namespace sncf {

std::string ExclusiveScope::to_key() const {
  std::string out;
  out.reserve(48U);
  out.append("sn=");
  out.append(std::to_string(smartnic.value()));
  out.append(";dev=");
  out.append(std::to_string(device.value()));
  if (port.valid()) {
    out.append(";port=");
    out.append(std::to_string(port.value()));
  } else {
    out.append(";port=*");
  }
  return out;
}

void FencingLedger::observe(const ExclusiveScope& scope, FencingToken token) noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), scope,
                                   [](const auto& entry, const ExclusiveScope& key) { return entry.first < key; });
  if (it != entries_.end() && it->first == scope) {
    if (token > it->second) {
      it->second = token;
    }
    return;
  }
  entries_.insert(it, {scope, token});
}

FencingToken FencingLedger::high_water(const ExclusiveScope& scope) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), scope,
                                   [](const auto& entry, const ExclusiveScope& key) { return entry.first < key; });
  if (it != entries_.end() && it->first == scope) {
    return it->second;
  }
  return FencingToken{};
}

std::optional<FencingToken> FencingLedger::advance(const ExclusiveScope& scope) noexcept {
  const FencingToken current = high_water(scope);
  if (current.value() == (std::numeric_limits<std::uint64_t>::max)()) {
    return std::nullopt;
  }
  const FencingToken next = FencingToken::from_value(current.value() + 1U);
  observe(scope, next);
  return next;
}

std::optional<FencingToken> FencingLedger::allocate(const ExclusiveScope& scope) noexcept {
  return advance(scope);
}

}  // namespace sncf
