// Copyright 2026 Summon Software Labs.
#include "sncf/explain.hpp"

namespace sncf {

Value ExplanationFactor::to_value() const {
  Value::object_type fields{
      {"detail", Value::string(detail)},
      {"reason", Value::string(std::string(reason_name(reason)))},
      {"subject", Value::string(subject)},
  };
  if (has_evidence) {
    fields.emplace_back("evidence_digest", Value::string(evidence_digest.to_hex()));
  }
  if (has_generation) {
    fields.emplace_back("generation", Value::uint_value(generation));
  }
  return Value::object(std::move(fields));
}

Value Explanation::to_value() const {
  Value::array_type items;
  items.reserve(factors.size());
  for (const auto& factor : factors) {
    items.push_back(factor.to_value());
  }
  return Value::object({
      {"accepted", Value::boolean(accepted)},
      {"at", Value::uint_value(at.value())},
      {"epoch", Value::uint_value(epoch.value())},
      {"factors", Value::array(std::move(items))},
      {"operation", Value::string(operation)},
      {"primary_reason", Value::string(std::string(reason_name(primary_reason)))},
      {"subject", Value::string(subject)},
  });
}

std::string Explanation::to_canonical() const { return to_value().to_canonical(); }

ExplanationFactor factor_of(ReasonCode reason, std::string subject, std::string detail) {
  ExplanationFactor factor;
  factor.reason = reason;
  factor.subject = std::move(subject);
  factor.detail = std::move(detail);
  return factor;
}

ExplanationFactor factor_with_evidence(ReasonCode reason, std::string subject, std::string detail,
                                       const Digest256& digest) {
  ExplanationFactor factor = factor_of(reason, std::move(subject), std::move(detail));
  factor.has_evidence = true;
  factor.evidence_digest = digest;
  return factor;
}

ExplanationFactor factor_with_generation(ReasonCode reason, std::string subject, std::string detail,
                                         std::uint64_t generation) {
  ExplanationFactor factor = factor_of(reason, std::move(subject), std::move(detail));
  factor.has_generation = true;
  factor.generation = generation;
  return factor;
}

}  // namespace sncf
