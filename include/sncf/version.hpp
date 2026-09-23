// SmartNIC Control Fabric - version and semantic contract constants.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_VERSION_HPP
#define SNCF_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace sncf {

/// Product version of the SmartNIC Control Fabric runtime.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Wire/persistence format version for the durable store (journal + snapshot).
/// Bumping this is a breaking change: older stores are refused, never guessed at.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// Framing version for the control-fabric transport protocol.
inline constexpr std::uint32_t kProtocolVersion = 1;

/// Semantic contract revision. Persisted alongside the store; a mismatch means
/// the durable records no longer mean what this build thinks they mean, and
/// recovery refuses rather than reinterpreting. Increment on any semantic change
/// to the persisted state model.
inline constexpr std::uint32_t kSemanticsRevision = 1;

/// Canonical export schema version.
inline constexpr std::uint32_t kExportSchemaVersion = 1;

std::string_view version_string() noexcept;

/// Stable identifier for the semantics contract, e.g. "sncf-semantics-1".
std::string_view semantics_id() noexcept;

}  // namespace sncf

#endif  // SNCF_VERSION_HPP
