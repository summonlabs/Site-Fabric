// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Evidence provenance and freshness.
//
// Every statement a member domain makes is accompanied by how it was learned
// and how long it stays meaningful. Site Fabric never promotes one provenance
// class to another: synthetic evidence is not measured evidence, and evidence
// reconstructed from disk after a restart is not a fresh observation.

#ifndef SITE_FABRIC_EVIDENCE_HPP
#define SITE_FABRIC_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "site_fabric/generation.hpp"
#include "site_fabric/limits.hpp"

namespace site_fabric {

/// How a statement was learned.
///
/// The ordering is descriptive, not a trust ranking that a caller may use to
/// let a weaker class stand in for a stronger one.
enum class Provenance {
  UNKNOWN = 0,
  MEASURED,
  REPORTED,
  DERIVED,
  ESTIMATED,
  SYNTHETIC,
  RECONSTRUCTED,
};

[[nodiscard]] const char* to_string(Provenance provenance);
[[nodiscard]] bool parse_provenance(std::string_view token, Provenance& out);

/// True when the provenance class is acceptable as physical site evidence.
///
/// SYNTHETIC and UNKNOWN are never acceptable for a physical site, and
/// RECONSTRUCTED is acceptable only because the freshness evaluator forces it
/// to be treated as stale until it is revalidated.
[[nodiscard]] bool is_physical_evidence(Provenance provenance) noexcept;

/// True when the provenance class survives a restart as live evidence. Only
/// SYNTHETIC does not need to survive, because it was never live.
[[nodiscard]] bool is_live_after_restart(Provenance provenance) noexcept;

/// The freshness verdict for one evidence record at one instant.
enum class EvidenceState {
  UNKNOWN = 0,
  FRESH,
  STALE,
  FUTURE,
  INVALID,
  SYNTHETIC_ONLY,
  RECONSTRUCTED_NEEDS_REVALIDATION,
};

[[nodiscard]] const char* to_string(EvidenceState state);

/// One evidence record.
///
/// observed_at_ms is a wall-clock instant supplied by the reporter; ttl_ms is
/// how long the reporter claims it stays meaningful. Both are bounded, and
/// neither is trusted beyond its bounds.
struct Evidence {
  Provenance provenance = Provenance::UNKNOWN;
  std::int64_t observed_at_ms = 0;
  std::uint64_t ttl_ms = 0;
  /// The exact source this evidence describes. A zero digest is permitted
  /// only for evidence attached to a declaration before it is sealed.
  SourceRef source;
  Digest payload_digest;

  Evidence() = default;

  [[nodiscard]] static Evidence measured(std::int64_t observed, std::uint64_t ttl);
  [[nodiscard]] static Evidence reported(std::int64_t observed, std::uint64_t ttl);
  [[nodiscard]] static Evidence synthetic(std::int64_t observed, std::uint64_t ttl);
  [[nodiscard]] static Evidence reconstructed(std::int64_t observed, std::uint64_t ttl);

  [[nodiscard]] bool valid() const noexcept;

  /// Freshness at an explicit instant. Passing the instant in, rather than
  /// reading a clock, is what makes composition reproducible: the same inputs
  /// evaluated at the same instant always yield the same verdict.
  [[nodiscard]] EvidenceState evaluate(std::int64_t now_ms) const;

  [[nodiscard]] std::string to_string() const;
};

/// Classification of how much of the expected evidence actually arrived.
enum class CompletenessClass {
  UNKNOWN = 0,
  COMPLETE,
  PARTIAL,
  MISSING,
};

[[nodiscard]] const char* to_string(CompletenessClass value);

}  // namespace site_fabric

#endif  // SITE_FABRIC_EVIDENCE_HPP
