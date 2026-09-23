// Deterministic explanation surface.
//
// Every accepted or refused decision produces an Explanation whose factors are
// emitted in a fixed pipeline order. The same inputs always produce byte-identical
// explanations, which is what makes "why was this refused" answerable offline.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_EXPLAIN_HPP
#define SNCF_EXPLAIN_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "sncf/canonical.hpp"
#include "sncf/digest.hpp"
#include "sncf/ids.hpp"
#include "sncf/status.hpp"

namespace sncf {

/// One step of a decision. A factor names the rule that applied, the entity it
/// applied to, and the evidence - by digest and generation - that made it legal
/// or illegal. Absent evidence is recorded as absent; it never becomes a zero
/// digest silently.
struct ExplanationFactor {
  ReasonCode reason = ReasonCode::Unknown;
  std::string subject;
  std::string detail;
  bool has_evidence = false;
  Digest256 evidence_digest;
  bool has_generation = false;
  std::uint64_t generation = 0;

  [[nodiscard]] Value to_value() const;
};

struct Explanation {
  std::string operation;
  std::string subject;
  bool accepted = false;
  ReasonCode primary_reason = ReasonCode::Unknown;
  std::vector<ExplanationFactor> factors;
  CoordinatorEpoch epoch;
  TimestampNs at;

  [[nodiscard]] Value to_value() const;
  [[nodiscard]] std::string to_canonical() const;
};

[[nodiscard]] ExplanationFactor factor_of(ReasonCode reason, std::string subject, std::string detail);
[[nodiscard]] ExplanationFactor factor_with_evidence(ReasonCode reason, std::string subject, std::string detail,
                                                     const Digest256& digest);
[[nodiscard]] ExplanationFactor factor_with_generation(ReasonCode reason, std::string subject,
                                                       std::string detail, std::uint64_t generation);

}  // namespace sncf

#endif  // SNCF_EXPLAIN_HPP
