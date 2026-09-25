// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Typed outcomes.
//
// Site Fabric never answers a question with a bare boolean. Every operation
// returns a Status drawn from the enumeration below, and every rejection
// carries bounded, stable factors that explain it. The distinctions that
// matter most are the ones a boolean would erase: UNKNOWN is not OK, STALE is
// not OK, and INDETERMINATE is not OK.

#ifndef SITE_FABRIC_STATUS_HPP
#define SITE_FABRIC_STATUS_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "site_fabric/limits.hpp"

namespace site_fabric {

/// The complete outcome vocabulary.
///
/// The ordering groups related codes; it is not a severity ordering and no
/// code may be derived from another by comparison.
enum class Status {
  UNKNOWN = 0,

  // --- Positive -----------------------------------------------------------
  OK,
  ACCEPTED,
  COMPOSED,
  CURRENT,
  PUBLISHED,
  RENEWED,
  GRANTED,

  // --- Knowledge states ---------------------------------------------------
  UNSUPPORTED,
  STALE,
  CONFLICTING,
  INCOMPLETE,
  INDETERMINATE,
  PARTIAL,
  DEGRADED,
  PARTITIONED,

  // --- Refusals -----------------------------------------------------------
  REFUSED,
  CANCELLED,
  INVALID,
  MALFORMED,
  TRUNCATED,
  CORRUPT,
  REPLAYED,
  NOT_FOUND,
  ALREADY_EXISTS,
  ALREADY_RETIRED,
  LIMIT_EXCEEDED,
  CAPACITY_OVERFLOW,
  CAPACITY_UNDERFLOW,
  UNSUPPORTED_FORMAT,
  SCHEMA_INCOMPATIBLE,
  DIGEST_MISMATCH,
  INTEGRITY_FAILURE,
  BUSY,
  STOPPED,
  NOT_STARTED,

  // --- Authority ----------------------------------------------------------
  UNAUTHORIZED,
  FENCED_EPOCH,
  FENCED_INCARNATION,
  FENCED_GENERATION,
  FENCED_ATTEMPT,
  LEASE_EXPIRED,
  LEASE_NOT_YET_VALID,
  LEASE_REVOKED,
  AUTHORITY_HELD_ELSEWHERE,

  // --- Composition findings ----------------------------------------------
  MEMBERSHIP_MISSING,
  MEMBERSHIP_UNEXPECTED,
  GENERATION_MISMATCH,
  DIGEST_MISMATCH_DECLARATION,
  SITE_MISMATCH,
  EPOCH_MISMATCH,
  OWNERSHIP_OVERLAP,
  ACCOUNTING_CONFLICT,
  CAPACITY_UNKNOWN,
  FAILURE_DOMAIN_CYCLE,
  FAILURE_DOMAIN_CONFLICT,
  FAILURE_DOMAIN_DANGLING_PARENT,
  MAINTENANCE_CONFLICT,
  MAINTENANCE_UNKNOWN,
  OBLIGATION_VIOLATED,
  OBLIGATION_INDETERMINATE,
  EVIDENCE_STALE,
  EVIDENCE_MISSING,
  EVIDENCE_SYNTHETIC_ONLY,
  EVIDENCE_FUTURE,
  CONNECTIVITY_UNKNOWN,
  CONNECTIVITY_DOWN,
  CONNECTIVITY_DEGRADED,
  EXPECTATION_MISMATCH,
  DEPENDENCY_INVALIDATED,
  NO_AUTHORITATIVE_SOURCE,
};

/// Renders a status as its stable upper-case token.
///
/// The token is part of the inspection and wire surface: it is written to
/// snapshots, printed by the tools, and asserted by the tests, so it must not
/// change once released.
[[nodiscard]] const char* to_string(Status status);

/// Parses the stable token back to a status. Returns Status::UNKNOWN for an
/// unrecognised token; callers that must distinguish "unrecognised" from
/// "UNKNOWN" use ::site_fabric::parse_status_exact.
[[nodiscard]] Status parse_status(const std::string& token);

/// Parses strictly: returns false for an unrecognised token instead of
/// collapsing it to Status::UNKNOWN.
[[nodiscard]] bool parse_status_exact(const std::string& token, Status& out);

/// True only for the positive codes. UNKNOWN, STALE, CONFLICTING, INCOMPLETE,
/// INDETERMINATE, PARTIAL and DEGRADED are all false.
[[nodiscard]] bool is_ok(Status status) noexcept;

/// True for codes that describe missing or unusable knowledge rather than a
/// refusal of a well-formed request.
[[nodiscard]] bool is_knowledge_gap(Status status) noexcept;

/// True for codes that a caller may retry unchanged without it being a defect.
[[nodiscard]] bool is_retryable(Status status) noexcept;

/// True for codes produced by authority fencing.
[[nodiscard]] bool is_fencing(Status status) noexcept;

/// Bounded factor list. Pushes beyond the bound are dropped and counted, so a
/// hostile input cannot grow an explanation without limit while the fact that
/// it tried is still visible.
class Factors {
 public:
  Factors() = default;

  void add(std::string factor);
  void add(std::string_view prefix, std::string_view value);

  [[nodiscard]] const std::vector<std::string>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

  [[nodiscard]] std::string join(std::string_view separator) const;
  [[nodiscard]] bool contains(std::string_view needle) const;

 private:
  std::vector<std::string> items_;
  std::size_t dropped_ = 0;
};

/// A status plus its explanation.
struct Outcome {
  Status status = Status::UNKNOWN;
  Factors factors;
  std::string detail;

  Outcome() = default;
  explicit Outcome(Status value) : status(value) {}
  Outcome(Status value, std::string detail_text)
      : status(value), detail(std::move(detail_text)) {}

  [[nodiscard]] bool ok() const noexcept { return is_ok(status); }

  [[nodiscard]] std::string to_string() const;

  static Outcome success(Status value = Status::OK) { return Outcome(value); }
  static Outcome failure(Status value, std::string detail_text = {}) {
    return Outcome(value, std::move(detail_text));
  }
};

/// Convenience for building an outcome with factors inline.
class OutcomeBuilder {
 public:
  explicit OutcomeBuilder(Status status) : outcome_(status) {}

  OutcomeBuilder& factor(std::string value) {
    outcome_.factors.add(std::move(value));
    return *this;
  }
  OutcomeBuilder& factor(std::string_view prefix, std::string_view value) {
    outcome_.factors.add(prefix, value);
    return *this;
  }
  OutcomeBuilder& detail(std::string value) {
    outcome_.detail = std::move(value);
    return *this;
  }

  [[nodiscard]] Outcome build() && { return std::move(outcome_); }
  [[nodiscard]] Outcome build() const& { return outcome_; }
  [[nodiscard]] Status status() const noexcept { return outcome_.status; }

 private:
  Outcome outcome_;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_STATUS_HPP
