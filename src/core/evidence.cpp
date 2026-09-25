// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "site_fabric/evidence.hpp"

#include <string>

namespace site_fabric {
namespace {

/// How far into the future a reporter's clock may run before its evidence is
/// treated as unusable rather than merely early. Five seconds absorbs ordinary
/// clock skew between processes on one host and rejects a fabricated future.
constexpr std::int64_t kFutureSkewMs = 5'000;

}  // namespace

Evidence Evidence::measured(std::int64_t observed, std::uint64_t ttl) {
  Evidence evidence;
  evidence.provenance = Provenance::MEASURED;
  evidence.observed_at_ms = observed;
  evidence.ttl_ms = ttl;
  return evidence;
}

Evidence Evidence::reported(std::int64_t observed, std::uint64_t ttl) {
  Evidence evidence;
  evidence.provenance = Provenance::REPORTED;
  evidence.observed_at_ms = observed;
  evidence.ttl_ms = ttl;
  return evidence;
}

Evidence Evidence::synthetic(std::int64_t observed, std::uint64_t ttl) {
  Evidence evidence;
  evidence.provenance = Provenance::SYNTHETIC;
  evidence.observed_at_ms = observed;
  evidence.ttl_ms = ttl;
  return evidence;
}

Evidence Evidence::reconstructed(std::int64_t observed, std::uint64_t ttl) {
  Evidence evidence;
  evidence.provenance = Provenance::RECONSTRUCTED;
  evidence.observed_at_ms = observed;
  evidence.ttl_ms = ttl;
  return evidence;
}

bool Evidence::valid() const noexcept {
  return provenance != Provenance::UNKNOWN && observed_at_ms > 0 && ttl_ms > 0 &&
         ttl_ms <= limits::kMaxEvidenceTtlMs;
}

EvidenceState Evidence::evaluate(std::int64_t now_ms) const {
  if (provenance == Provenance::UNKNOWN) {
    return EvidenceState::UNKNOWN;
  }
  if (observed_at_ms <= 0 || ttl_ms == 0 || ttl_ms > limits::kMaxEvidenceTtlMs) {
    return EvidenceState::INVALID;
  }
  if (observed_at_ms > now_ms + kFutureSkewMs) {
    return EvidenceState::FUTURE;
  }
  if (provenance == Provenance::SYNTHETIC) {
    return EvidenceState::SYNTHETIC_ONLY;
  }
  if (provenance == Provenance::RECONSTRUCTED) {
    return EvidenceState::RECONSTRUCTED_NEEDS_REVALIDATION;
  }
  if (observed_at_ms + static_cast<std::int64_t>(ttl_ms) < now_ms) {
    return EvidenceState::STALE;
  }
  return EvidenceState::FRESH;
}

std::string Evidence::to_string() const {
  std::string out = site_fabric::to_string(provenance);
  out += "@";
  out += std::to_string(observed_at_ms);
  out += "+";
  out += std::to_string(ttl_ms);
  return out;
}

}  // namespace site_fabric
