// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Model primitives: identifiers, generations, digests, statuses and the stable
// token tables that the wire format, the persistence format and the tools all
// share. Changing a token here changes a released interface, so every table is
// exhaustive and every parse is strict.

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "internal/crypto.hpp"
#include "site_fabric/failure_domain.hpp"
#include "site_fabric/site_fabric.hpp"

namespace site_fabric {

namespace {

using TokenPair = std::pair<Status, const char*>;

// The order of this table is the order the tools print, and it is stable.
constexpr TokenPair kStatusTokens[] = {
    {Status::UNKNOWN, "UNKNOWN"},
    {Status::OK, "OK"},
    {Status::ACCEPTED, "ACCEPTED"},
    {Status::COMPOSED, "COMPOSED"},
    {Status::CURRENT, "CURRENT"},
    {Status::PUBLISHED, "PUBLISHED"},
    {Status::RENEWED, "RENEWED"},
    {Status::GRANTED, "GRANTED"},
    {Status::UNSUPPORTED, "UNSUPPORTED"},
    {Status::STALE, "STALE"},
    {Status::CONFLICTING, "CONFLICTING"},
    {Status::INCOMPLETE, "INCOMPLETE"},
    {Status::INDETERMINATE, "INDETERMINATE"},
    {Status::PARTIAL, "PARTIAL"},
    {Status::DEGRADED, "DEGRADED"},
    {Status::PARTITIONED, "PARTITIONED"},
    {Status::REFUSED, "REFUSED"},
    {Status::CANCELLED, "CANCELLED"},
    {Status::INVALID, "INVALID"},
    {Status::MALFORMED, "MALFORMED"},
    {Status::TRUNCATED, "TRUNCATED"},
    {Status::CORRUPT, "CORRUPT"},
    {Status::REPLAYED, "REPLAYED"},
    {Status::NOT_FOUND, "NOT_FOUND"},
    {Status::ALREADY_EXISTS, "ALREADY_EXISTS"},
    {Status::ALREADY_RETIRED, "ALREADY_RETIRED"},
    {Status::LIMIT_EXCEEDED, "LIMIT_EXCEEDED"},
    {Status::CAPACITY_OVERFLOW, "CAPACITY_OVERFLOW"},
    {Status::CAPACITY_UNDERFLOW, "CAPACITY_UNDERFLOW"},
    {Status::UNSUPPORTED_FORMAT, "UNSUPPORTED_FORMAT"},
    {Status::SCHEMA_INCOMPATIBLE, "SCHEMA_INCOMPATIBLE"},
    {Status::DIGEST_MISMATCH, "DIGEST_MISMATCH"},
    {Status::INTEGRITY_FAILURE, "INTEGRITY_FAILURE"},
    {Status::BUSY, "BUSY"},
    {Status::STOPPED, "STOPPED"},
    {Status::NOT_STARTED, "NOT_STARTED"},
    {Status::UNAUTHORIZED, "UNAUTHORIZED"},
    {Status::FENCED_EPOCH, "FENCED_EPOCH"},
    {Status::FENCED_INCARNATION, "FENCED_INCARNATION"},
    {Status::FENCED_GENERATION, "FENCED_GENERATION"},
    {Status::FENCED_ATTEMPT, "FENCED_ATTEMPT"},
    {Status::LEASE_EXPIRED, "LEASE_EXPIRED"},
    {Status::LEASE_NOT_YET_VALID, "LEASE_NOT_YET_VALID"},
    {Status::LEASE_REVOKED, "LEASE_REVOKED"},
    {Status::AUTHORITY_HELD_ELSEWHERE, "AUTHORITY_HELD_ELSEWHERE"},
    {Status::MEMBERSHIP_MISSING, "MEMBERSHIP_MISSING"},
    {Status::MEMBERSHIP_UNEXPECTED, "MEMBERSHIP_UNEXPECTED"},
    {Status::GENERATION_MISMATCH, "GENERATION_MISMATCH"},
    {Status::DIGEST_MISMATCH_DECLARATION, "DIGEST_MISMATCH_DECLARATION"},
    {Status::SITE_MISMATCH, "SITE_MISMATCH"},
    {Status::EPOCH_MISMATCH, "EPOCH_MISMATCH"},
    {Status::OWNERSHIP_OVERLAP, "OWNERSHIP_OVERLAP"},
    {Status::ACCOUNTING_CONFLICT, "ACCOUNTING_CONFLICT"},
    {Status::CAPACITY_UNKNOWN, "CAPACITY_UNKNOWN"},
    {Status::FAILURE_DOMAIN_CYCLE, "FAILURE_DOMAIN_CYCLE"},
    {Status::FAILURE_DOMAIN_CONFLICT, "FAILURE_DOMAIN_CONFLICT"},
    {Status::FAILURE_DOMAIN_DANGLING_PARENT, "FAILURE_DOMAIN_DANGLING_PARENT"},
    {Status::MAINTENANCE_CONFLICT, "MAINTENANCE_CONFLICT"},
    {Status::MAINTENANCE_UNKNOWN, "MAINTENANCE_UNKNOWN"},
    {Status::OBLIGATION_VIOLATED, "OBLIGATION_VIOLATED"},
    {Status::OBLIGATION_INDETERMINATE, "OBLIGATION_INDETERMINATE"},
    {Status::EVIDENCE_STALE, "EVIDENCE_STALE"},
    {Status::EVIDENCE_MISSING, "EVIDENCE_MISSING"},
    {Status::EVIDENCE_SYNTHETIC_ONLY, "EVIDENCE_SYNTHETIC_ONLY"},
    {Status::EVIDENCE_FUTURE, "EVIDENCE_FUTURE"},
    {Status::CONNECTIVITY_UNKNOWN, "CONNECTIVITY_UNKNOWN"},
    {Status::CONNECTIVITY_DOWN, "CONNECTIVITY_DOWN"},
    {Status::CONNECTIVITY_DEGRADED, "CONNECTIVITY_DEGRADED"},
    {Status::EXPECTATION_MISMATCH, "EXPECTATION_MISMATCH"},
    {Status::DEPENDENCY_INVALIDATED, "DEPENDENCY_INVALIDATED"},
    {Status::NO_AUTHORITATIVE_SOURCE, "NO_AUTHORITATIVE_SOURCE"},
};

template <class Enum>
[[nodiscard]] const char* lookup(std::span<const std::pair<Enum, const char*>> table,
                                 Enum value) {
  for (const auto& entry : table) {
    if (entry.first == value) {
      return entry.second != nullptr ? entry.second : "UNKNOWN";
    }
  }
  return "UNKNOWN";
}

template <class Enum>
[[nodiscard]] bool reverse_lookup(std::span<const std::pair<Enum, const char*>> table,
                                  std::string_view token, Enum& out) {
  for (const auto& entry : table) {
    if (entry.second != nullptr && token == entry.second) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

constexpr std::pair<MemberDomainKind, const char*> kMemberDomainKindTokens[] = {
    {MemberDomainKind::UNKNOWN, "UNKNOWN"},
    {MemberDomainKind::CLUSTER, "CLUSTER"},
    {MemberDomainKind::POD, "POD"},
    {MemberDomainKind::RACK, "RACK"},
    {MemberDomainKind::SHARED_RESOURCE, "SHARED_RESOURCE"},
};

constexpr std::pair<ResourceKind, const char*> kResourceKindTokens[] = {
    {ResourceKind::UNKNOWN, "UNKNOWN"},
    {ResourceKind::OBLIGATION, "OBLIGATION"},
    {ResourceKind::CLUSTER, "CLUSTER"},
    {ResourceKind::POD, "POD"},
    {ResourceKind::RACK, "RACK"},
    {ResourceKind::SHARED_LINK, "SHARED_LINK"},
    {ResourceKind::GATEWAY, "GATEWAY"},
    {ResourceKind::CAPACITY_POOL, "CAPACITY_POOL"},
    {ResourceKind::FAILURE_DOMAIN, "FAILURE_DOMAIN"},
    {ResourceKind::MAINTENANCE_ZONE, "MAINTENANCE_ZONE"},
};

constexpr std::pair<Provenance, const char*> kProvenanceTokens[] = {
    {Provenance::UNKNOWN, "UNKNOWN"},
    {Provenance::MEASURED, "MEASURED"},
    {Provenance::REPORTED, "REPORTED"},
    {Provenance::DERIVED, "DERIVED"},
    {Provenance::ESTIMATED, "ESTIMATED"},
    {Provenance::SYNTHETIC, "SYNTHETIC"},
    {Provenance::RECONSTRUCTED, "RECONSTRUCTED"},
};

constexpr std::pair<EvidenceState, const char*> kEvidenceStateTokens[] = {
    {EvidenceState::UNKNOWN, "UNKNOWN"},
    {EvidenceState::FRESH, "FRESH"},
    {EvidenceState::STALE, "STALE"},
    {EvidenceState::FUTURE, "FUTURE"},
    {EvidenceState::INVALID, "INVALID"},
    {EvidenceState::SYNTHETIC_ONLY, "SYNTHETIC_ONLY"},
    {EvidenceState::RECONSTRUCTED_NEEDS_REVALIDATION, "RECONSTRUCTED_NEEDS_REVALIDATION"},
};

constexpr std::pair<CompletenessClass, const char*> kCompletenessTokens[] = {
    {CompletenessClass::UNKNOWN, "UNKNOWN"},
    {CompletenessClass::COMPLETE, "COMPLETE"},
    {CompletenessClass::PARTIAL, "PARTIAL"},
    {CompletenessClass::MISSING, "MISSING"},
};

constexpr std::pair<CapacityChannel, const char*> kChannelTokens[] = {
    {CapacityChannel::UNKNOWN, "UNKNOWN"},
    {CapacityChannel::INGRESS, "INGRESS"},
    {CapacityChannel::EGRESS, "EGRESS"},
    {CapacityChannel::INTERNAL, "INTERNAL"},
};

constexpr std::pair<CapacityScope, const char*> kScopeTokens[] = {
    {CapacityScope::UNKNOWN, "UNKNOWN"},
    {CapacityScope::SITE_INGRESS, "SITE_INGRESS"},
    {CapacityScope::SITE_EGRESS, "SITE_EGRESS"},
    {CapacityScope::SITE_INTERNAL, "SITE_INTERNAL"},
    {CapacityScope::RACK_LOCAL, "RACK_LOCAL"},
    {CapacityScope::POD_LOCAL, "POD_LOCAL"},
    {CapacityScope::CLUSTER_LOCAL, "CLUSTER_LOCAL"},
    {CapacityScope::SHARED_LINK, "SHARED_LINK"},
    {CapacityScope::GATEWAY, "GATEWAY"},
};

constexpr std::pair<ConnectivityState, const char*> kConnectivityTokens[] = {
    {ConnectivityState::UNKNOWN, "UNKNOWN"},
    {ConnectivityState::UP, "UP"},
    {ConnectivityState::DOWN, "DOWN"},
    {ConnectivityState::DEGRADED, "DEGRADED"},
};

constexpr std::pair<FailureDomainClass, const char*> kDomainClassTokens[] = {
    {FailureDomainClass::UNKNOWN, "UNKNOWN"},
    {FailureDomainClass::PHYSICAL, "PHYSICAL"},
    {FailureDomainClass::ADMINISTRATIVE, "ADMINISTRATIVE"},
};

constexpr std::pair<MaintenanceState, const char*> kMaintenanceStateTokens[] = {
    {MaintenanceState::UNKNOWN, "UNKNOWN"},
    {MaintenanceState::SCHEDULED, "SCHEDULED"},
    {MaintenanceState::ACTIVE, "ACTIVE"},
    {MaintenanceState::COMPLETE, "COMPLETE"},
    {MaintenanceState::CANCELLED, "CANCELLED"},
};

constexpr std::pair<ObligationKind, const char*> kObligationKindTokens[] = {
    {ObligationKind::UNKNOWN, "UNKNOWN"},
    {ObligationKind::MIN_INGRESS_CAPACITY, "MIN_INGRESS_CAPACITY"},
    {ObligationKind::MIN_EGRESS_CAPACITY, "MIN_EGRESS_CAPACITY"},
    {ObligationKind::MIN_INTERNAL_CAPACITY, "MIN_INTERNAL_CAPACITY"},
    {ObligationKind::MIN_DISTINCT_FAILURE_DOMAINS, "MIN_DISTINCT_FAILURE_DOMAINS"},
    {ObligationKind::MIN_UP_SHARED_LINKS, "MIN_UP_SHARED_LINKS"},
    {ObligationKind::MIN_UP_GATEWAYS, "MIN_UP_GATEWAYS"},
    {ObligationKind::DOMAIN_SURVIVABILITY, "DOMAIN_SURVIVABILITY"},
};

constexpr std::pair<MemberLifecycle, const char*> kMemberLifecycleTokens[] = {
    {MemberLifecycle::UNKNOWN, "UNKNOWN"},
    {MemberLifecycle::ABSENT, "ABSENT"},
    {MemberLifecycle::REPORTED, "REPORTED"},
    {MemberLifecycle::CURRENT, "CURRENT"},
    {MemberLifecycle::STALE, "STALE"},
    {MemberLifecycle::SUPERSEDED, "SUPERSEDED"},
    {MemberLifecycle::CONFLICTING, "CONFLICTING"},
    {MemberLifecycle::INCOMPATIBLE, "INCOMPATIBLE"},
    {MemberLifecycle::RETIRED, "RETIRED"},
    {MemberLifecycle::REVOKED, "REVOKED"},
};

constexpr std::pair<SiteLifecycle, const char*> kSiteLifecycleTokens[] = {
    {SiteLifecycle::UNKNOWN, "UNKNOWN"},
    {SiteLifecycle::INITIALIZING, "INITIALIZING"},
    {SiteLifecycle::CURRENT, "CURRENT"},
    {SiteLifecycle::DEGRADED, "DEGRADED"},
    {SiteLifecycle::PARTITIONED, "PARTITIONED"},
    {SiteLifecycle::INCOMPLETE, "INCOMPLETE"},
    {SiteLifecycle::CONFLICTING, "CONFLICTING"},
    {SiteLifecycle::INDETERMINATE, "INDETERMINATE"},
    {SiteLifecycle::STOPPED, "STOPPED"},
};

constexpr std::pair<DecisionKind, const char*> kDecisionKindTokens[] = {
    {DecisionKind::UNKNOWN, "UNKNOWN"},
    {DecisionKind::MEMBERSHIP_COMPLETENESS, "MEMBERSHIP_COMPLETENESS"},
    {DecisionKind::SITE_EPOCH, "SITE_EPOCH"},
    {DecisionKind::OWNERSHIP_MAP, "OWNERSHIP_MAP"},
    {DecisionKind::RACK_TOPOLOGY, "RACK_TOPOLOGY"},
    {DecisionKind::FAILURE_DOMAIN_FOREST, "FAILURE_DOMAIN_FOREST"},
    {DecisionKind::CONNECTIVITY_SUMMARY, "CONNECTIVITY_SUMMARY"},
    {DecisionKind::SHARED_RESOURCE_ACCOUNTING, "SHARED_RESOURCE_ACCOUNTING"},
    {DecisionKind::CAPACITY_TOTAL, "CAPACITY_TOTAL"},
    {DecisionKind::CAPACITY_AVAILABLE, "CAPACITY_AVAILABLE"},
    {DecisionKind::MAINTENANCE_EXCLUSION, "MAINTENANCE_EXCLUSION"},
    {DecisionKind::OBLIGATION_VERDICT, "OBLIGATION_VERDICT"},
    {DecisionKind::SITE_LIFECYCLE, "SITE_LIFECYCLE"},
};

constexpr std::pair<AuthorityKind, const char*> kAuthorityKindTokens[] = {
    {AuthorityKind::UNKNOWN, "UNKNOWN"},
    {AuthorityKind::SITE_CONTROLLER, "SITE_CONTROLLER"},
    {AuthorityKind::MEMBER_PUBLISHER, "MEMBER_PUBLISHER"},
    {AuthorityKind::MAINTENANCE_OPERATOR, "MAINTENANCE_OPERATOR"},
    {AuthorityKind::OBSERVER, "OBSERVER"},
};

constexpr std::pair<LeaseState, const char*> kLeaseStateTokens[] = {
    {LeaseState::UNKNOWN, "UNKNOWN"},
    {LeaseState::ACTIVE, "ACTIVE"},
    {LeaseState::EXPIRED, "EXPIRED"},
    {LeaseState::REVOKED, "REVOKED"},
    {LeaseState::SUPERSEDED, "SUPERSEDED"},
};

constexpr std::pair<RecordType, const char*> kRecordTypeTokens[] = {
    {RecordType::UNKNOWN, "UNKNOWN"},
    {RecordType::STORE_HEADER, "STORE_HEADER"},
    {RecordType::SNAPSHOT, "SNAPSHOT"},
    {RecordType::PUBLICATION, "PUBLICATION"},
    {RecordType::RETIREMENT, "RETIREMENT"},
    {RecordType::TOMBSTONE, "TOMBSTONE"},
    {RecordType::LEASE, "LEASE"},
    {RecordType::CHECKPOINT, "CHECKPOINT"},
};

constexpr std::pair<MessageType, const char*> kMessageTypeTokens[] = {
    {MessageType::UNKNOWN, "UNKNOWN"},
    {MessageType::HELLO_REQUEST, "HELLO_REQUEST"},
    {MessageType::HELLO_RESPONSE, "HELLO_RESPONSE"},
    {MessageType::PUBLISH_REQUEST, "PUBLISH_REQUEST"},
    {MessageType::PUBLISH_RESPONSE, "PUBLISH_RESPONSE"},
    {MessageType::RETIRE_REQUEST, "RETIRE_REQUEST"},
    {MessageType::RETIRE_RESPONSE, "RETIRE_RESPONSE"},
    {MessageType::FETCH_SITE_REQUEST, "FETCH_SITE_REQUEST"},
    {MessageType::FETCH_SITE_RESPONSE, "FETCH_SITE_RESPONSE"},
    {MessageType::HEARTBEAT_REQUEST, "HEARTBEAT_REQUEST"},
    {MessageType::HEARTBEAT_RESPONSE, "HEARTBEAT_RESPONSE"},
    {MessageType::INVALIDATE_NOTICE, "INVALIDATE_NOTICE"},
    {MessageType::REVOKE_NOTICE, "REVOKE_NOTICE"},
    {MessageType::ERROR_RESPONSE, "ERROR_RESPONSE"},
    {MessageType::SHUTDOWN_REQUEST, "SHUTDOWN_REQUEST"},
    {MessageType::SHUTDOWN_RESPONSE, "SHUTDOWN_RESPONSE"},
    {MessageType::PING, "PING"},
    {MessageType::PONG, "PONG"},
};

constexpr std::array<Status, 8> kOkStatuses = {
    Status::OK,       Status::ACCEPTED, Status::COMPOSED, Status::CURRENT,
    Status::PUBLISHED, Status::RENEWED,  Status::GRANTED,  Status::ALREADY_EXISTS};

}  // namespace

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

const char* to_string(Status status) { return lookup<Status>(kStatusTokens, status); }

Status parse_status(const std::string& token) {
  Status parsed = Status::UNKNOWN;
  if (reverse_lookup<Status>(kStatusTokens, token, parsed)) {
    return parsed;
  }
  return Status::UNKNOWN;
}

bool parse_status_exact(const std::string& token, Status& out) {
  return reverse_lookup<Status>(kStatusTokens, token, out);
}

bool is_ok(Status status) noexcept {
  return std::find(kOkStatuses.begin(), kOkStatuses.end(), status) != kOkStatuses.end();
}

bool is_knowledge_gap(Status status) noexcept {
  switch (status) {
    case Status::UNKNOWN:
    case Status::STALE:
    case Status::CONFLICTING:
    case Status::INCOMPLETE:
    case Status::INDETERMINATE:
    case Status::PARTIAL:
    case Status::DEGRADED:
    case Status::PARTITIONED:
    case Status::CAPACITY_UNKNOWN:
    case Status::EVIDENCE_MISSING:
    case Status::EVIDENCE_STALE:
    case Status::EVIDENCE_SYNTHETIC_ONLY:
    case Status::EVIDENCE_FUTURE:
    case Status::CONNECTIVITY_UNKNOWN:
    case Status::MAINTENANCE_UNKNOWN:
    case Status::MEMBERSHIP_MISSING:
    case Status::NO_AUTHORITATIVE_SOURCE:
      return true;
    default:
      return false;
  }
}

bool is_retryable(Status status) noexcept {
  switch (status) {
    case Status::BUSY:
    case Status::NOT_STARTED:
    case Status::REFUSED:
    case Status::INCOMPLETE:
    case Status::PARTIAL:
      return true;
    default:
      return false;
  }
}

bool is_fencing(Status status) noexcept {
  switch (status) {
    case Status::FENCED_EPOCH:
    case Status::FENCED_INCARNATION:
    case Status::FENCED_GENERATION:
    case Status::FENCED_ATTEMPT:
    case Status::LEASE_EXPIRED:
    case Status::LEASE_NOT_YET_VALID:
    case Status::LEASE_REVOKED:
    case Status::AUTHORITY_HELD_ELSEWHERE:
    case Status::UNAUTHORIZED:
      return true;
    default:
      return false;
  }
}

void Factors::add(std::string factor) {
  if (items_.size() >= limits::kMaxFactors) {
    ++dropped_;
    return;
  }
  items_.push_back(std::move(factor));
}

void Factors::add(std::string_view prefix, std::string_view value) {
  std::string combined;
  combined.reserve(prefix.size() + value.size() + 1);
  combined.append(prefix);
  combined.push_back('=');
  combined.append(value);
  add(std::move(combined));
}

std::string Factors::join(std::string_view separator) const {
  std::string out;
  for (std::size_t index = 0; index < items_.size(); ++index) {
    if (index != 0) {
      out.append(separator);
    }
    out.append(items_[index]);
  }
  if (dropped_ != 0) {
    if (!out.empty()) {
      out.append(separator);
    }
    out += "dropped=" + std::to_string(dropped_);
  }
  return out;
}

bool Factors::contains(std::string_view needle) const {
  for (const auto& item : items_) {
    if (item.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string Outcome::to_string() const {
  std::string out = site_fabric::to_string(status);
  if (!detail.empty()) {
    out += " (";
    out += detail;
    out += ")";
  }
  if (!factors.empty()) {
    out += " [";
    out += factors.join(", ");
    out += "]";
  }
  return out;
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

bool is_valid_identifier(std::string_view text) noexcept {
  if (text.empty() || text.size() > limits::kMaxIdentifierBytes) {
    return false;
  }
  const auto is_lead = [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  const auto is_tail = [&is_lead](char character) {
    if (is_lead(character)) {
      return true;
    }
    switch (character) {
      case '.': case '_': case '-': case ':':
        return true;
      default:
        return false;
    }
  };
  if (!is_lead(text.front())) {
    return false;
  }
  for (const char character : text) {
    if (!is_tail(character)) {
      return false;
    }
  }
  // A trailing separator is refused: it makes prefix comparisons ambiguous.
  const char last = text.back();
  return last != ':' && last != '.' && last != '_' && last != '-';
}

const char* to_string(MemberDomainKind kind) {
  return lookup<MemberDomainKind>(kMemberDomainKindTokens, kind);
}

bool parse_member_domain_kind(std::string_view token, MemberDomainKind& out) {
  return reverse_lookup<MemberDomainKind>(
      kMemberDomainKindTokens, token, out);
}

bool is_valid(MemberDomainKind kind) noexcept {
  switch (kind) {
    case MemberDomainKind::CLUSTER:
    case MemberDomainKind::POD:
    case MemberDomainKind::RACK:
    case MemberDomainKind::SHARED_RESOURCE:
      return true;
    default:
      return false;
  }
}

std::string MemberDomainKey::to_string() const {
  std::string out = site_fabric::to_string(kind);
  out.push_back(':');
  out.append(id);
  return out;
}

bool MemberDomainKey::parse(std::string_view text, MemberDomainKey& out) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return false;
  }
  MemberDomainKind kind = MemberDomainKind::UNKNOWN;
  if (!parse_member_domain_kind(text.substr(0, separator), kind)) {
    return false;
  }
  if (!is_valid(kind)) {
    return false;
  }
  const std::string_view id = text.substr(separator + 1);
  if (!is_valid_identifier(id)) {
    return false;
  }
  out.kind = kind;
  out.id.assign(id);
  return true;
}

const char* to_string(ResourceKind kind) {
  return lookup<ResourceKind>(kResourceKindTokens, kind);
}

bool parse_resource_kind(std::string_view token, ResourceKind& out) {
  return reverse_lookup<ResourceKind>(kResourceKindTokens, token, out);
}

bool is_valid(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::CLUSTER:
    case ResourceKind::POD:
    case ResourceKind::RACK:
    case ResourceKind::SHARED_LINK:
    case ResourceKind::GATEWAY:
    case ResourceKind::CAPACITY_POOL:
    case ResourceKind::FAILURE_DOMAIN:
    case ResourceKind::MAINTENANCE_ZONE:
    case ResourceKind::OBLIGATION:
      return true;
    default:
      return false;
  }
}

std::string ResourceKey::to_string() const {
  std::string out = site_fabric::to_string(kind);
  out.push_back(':');
  out.append(id);
  return out;
}

bool ResourceKey::parse(std::string_view text, ResourceKey& out) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return false;
  }
  ResourceKind kind = ResourceKind::UNKNOWN;
  if (!parse_resource_kind(text.substr(0, separator), kind)) {
    return false;
  }
  if (!is_valid(kind)) {
    return false;
  }
  const std::string_view id = text.substr(separator + 1);
  if (!is_valid_identifier(id)) {
    return false;
  }
  out.kind = kind;
  out.id.assign(id);
  return true;
}

// ---------------------------------------------------------------------------
// Generations, epochs, incarnations, digests
// ---------------------------------------------------------------------------

Generation Generation::next() const {
  if (value_ == max_value()) {
    return *this;
  }
  return Generation(value_ + 1);
}

std::string Generation::to_string() const { return std::to_string(value_); }

Epoch Epoch::next() const {
  if (value_ == max_value()) {
    return *this;
  }
  return Epoch(value_ + 1);
}

std::string Epoch::to_string() const { return std::to_string(value_); }

std::string Incarnation::to_string() const {
  std::string out = std::to_string(sequence);
  out.push_back('/');
  out.append(boot.value);
  return out;
}

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest::to_string() const {
  return internal::to_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

std::string Digest::short_hex() const { return to_string().substr(0, 16); }

bool Digest::parse(std::string_view text, Digest& out) {
  if (text.size() != out.bytes.size() * 2) {
    return false;
  }
  std::vector<std::uint8_t> decoded;
  if (!internal::from_hex(text, decoded) || decoded.size() != out.bytes.size()) {
    return false;
  }
  std::copy(decoded.begin(), decoded.end(), out.bytes.begin());
  return true;
}

std::string SchemaVersion::to_string() const {
  return std::to_string(major) + "." + std::to_string(minor);
}

std::string SourceRef::to_string() const {
  return domain.to_string() + "@" + generation.to_string() + "#" + digest.short_hex();
}

void SourceSet::insert(const SourceRef& ref) {
  const auto position = std::lower_bound(items_.begin(), items_.end(), ref);
  if (position != items_.end() && *position == ref) {
    return;
  }
  if (items_.size() >= limits::kMaxDecisionSources) {
    truncated_ = true;
    ++dropped_;
    return;
  }
  items_.insert(position, ref);
}

void SourceSet::insert(const SourceSet& other) {
  for (const auto& ref : other.items_) {
    insert(ref);
  }
  if (other.truncated_) {
    truncated_ = true;
    dropped_ += other.dropped_;
  }
}

bool SourceSet::contains(const SourceRef& ref) const {
  return std::binary_search(items_.begin(), items_.end(), ref);
}

bool SourceSet::contains_domain(const MemberDomainKey& key) const {
  for (const auto& ref : items_) {
    if (ref.domain == key) {
      return true;
    }
  }
  return false;
}

bool SourceSet::intersects_difference(const SourceSet& other) const {
  for (const auto& ref : items_) {
    if (!other.contains(ref)) {
      return true;
    }
  }
  return false;
}

std::string SourceSet::to_string() const {
  std::string out;
  out.push_back('[');
  for (std::size_t index = 0; index < items_.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    out.append(items_[index].to_string());
  }
  out.push_back(']');
  if (truncated_) {
    out += "+truncated";
  }
  return out;
}

// ---------------------------------------------------------------------------
// Enum tokens
// ---------------------------------------------------------------------------

const char* to_string(Provenance provenance) {
  return lookup<Provenance>(kProvenanceTokens, provenance);
}

bool parse_provenance(std::string_view token, Provenance& out) {
  return reverse_lookup<Provenance>(kProvenanceTokens, token, out);
}

bool is_physical_evidence(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::MEASURED:
    case Provenance::REPORTED:
    case Provenance::DERIVED:
    case Provenance::ESTIMATED:
    case Provenance::RECONSTRUCTED:
      return true;
    default:
      return false;
  }
}

bool is_live_after_restart(Provenance provenance) noexcept {
  return provenance != Provenance::SYNTHETIC && provenance != Provenance::UNKNOWN;
}

const char* to_string(EvidenceState state) {
  return lookup<EvidenceState>(kEvidenceStateTokens, state);
}

const char* to_string(CompletenessClass value) {
  return lookup<CompletenessClass>(kCompletenessTokens, value);
}

const char* to_string(CapacityChannel channel) {
  return lookup<CapacityChannel>(kChannelTokens, channel);
}

bool parse_capacity_channel(std::string_view token, CapacityChannel& out) {
  return reverse_lookup<CapacityChannel>(kChannelTokens, token, out);
}

const char* to_string(CapacityScope scope) {
  return lookup<CapacityScope>(kScopeTokens, scope);
}

bool parse_capacity_scope(std::string_view token, CapacityScope& out) {
  return reverse_lookup<CapacityScope>(kScopeTokens, token, out);
}

bool is_valid(CapacityScope scope) noexcept {
  switch (scope) {
    case CapacityScope::SITE_INGRESS:
    case CapacityScope::SITE_EGRESS:
    case CapacityScope::SITE_INTERNAL:
    case CapacityScope::RACK_LOCAL:
    case CapacityScope::POD_LOCAL:
    case CapacityScope::CLUSTER_LOCAL:
    case CapacityScope::SHARED_LINK:
    case CapacityScope::GATEWAY:
      return true;
    default:
      return false;
  }
}

bool scope_feeds_site_total(CapacityScope scope) noexcept {
  switch (scope) {
    case CapacityScope::SITE_INGRESS:
    case CapacityScope::SITE_EGRESS:
    case CapacityScope::SITE_INTERNAL:
    case CapacityScope::RACK_LOCAL:
    case CapacityScope::POD_LOCAL:
    case CapacityScope::CLUSTER_LOCAL:
    case CapacityScope::SHARED_LINK:
    case CapacityScope::GATEWAY:
      return true;
    default:
      return false;
  }
}

const char* to_string(ConnectivityState state) {
  return lookup<ConnectivityState>(kConnectivityTokens, state);
}

bool parse_connectivity_state(std::string_view token, ConnectivityState& out) {
  return reverse_lookup<ConnectivityState>(kConnectivityTokens, token,
                                                                      out);
}

bool is_operational(ConnectivityState state) noexcept { return state == ConnectivityState::UP; }

const char* to_string(FailureDomainClass domain_class) {
  return lookup<FailureDomainClass>(kDomainClassTokens, domain_class);
}

bool parse_failure_domain_class(std::string_view token, FailureDomainClass& out) {
  return reverse_lookup<FailureDomainClass>(kDomainClassTokens, token,
                                                                      out);
}

bool is_valid(FailureDomainClass domain_class) noexcept {
  return domain_class == FailureDomainClass::PHYSICAL ||
         domain_class == FailureDomainClass::ADMINISTRATIVE;
}

const char* to_string(MaintenanceState state) {
  return lookup<MaintenanceState>(kMaintenanceStateTokens, state);
}

bool parse_maintenance_state(std::string_view token, MaintenanceState& out) {
  return reverse_lookup<MaintenanceState>(kMaintenanceStateTokens,
                                                                         token, out);
}

bool excludes_capacity(MaintenanceState state) noexcept { return state == MaintenanceState::ACTIVE; }

bool makes_indeterminate(MaintenanceState state) noexcept {
  return state == MaintenanceState::UNKNOWN;
}

const char* to_string(ObligationKind kind) {
  return lookup<ObligationKind>(kObligationKindTokens, kind);
}

bool parse_obligation_kind(std::string_view token, ObligationKind& out) {
  return reverse_lookup<ObligationKind>(kObligationKindTokens, token,
                                                                     out);
}

bool is_valid(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::MIN_INGRESS_CAPACITY:
    case ObligationKind::MIN_EGRESS_CAPACITY:
    case ObligationKind::MIN_INTERNAL_CAPACITY:
    case ObligationKind::MIN_DISTINCT_FAILURE_DOMAINS:
    case ObligationKind::MIN_UP_SHARED_LINKS:
    case ObligationKind::MIN_UP_GATEWAYS:
    case ObligationKind::DOMAIN_SURVIVABILITY:
      return true;
    default:
      return false;
  }
}

bool obligation_is_count(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::MIN_DISTINCT_FAILURE_DOMAINS:
    case ObligationKind::MIN_UP_SHARED_LINKS:
    case ObligationKind::MIN_UP_GATEWAYS:
    case ObligationKind::DOMAIN_SURVIVABILITY:
      return true;
    default:
      return false;
  }
}

CapacityChannel obligation_channel(ObligationKind kind) noexcept {
  switch (kind) {
    case ObligationKind::MIN_INGRESS_CAPACITY:
      return CapacityChannel::INGRESS;
    case ObligationKind::MIN_EGRESS_CAPACITY:
      return CapacityChannel::EGRESS;
    case ObligationKind::MIN_INTERNAL_CAPACITY:
      return CapacityChannel::INTERNAL;
    default:
      return CapacityChannel::UNKNOWN;
  }
}

const char* to_string(MemberLifecycle lifecycle) {
  return lookup<MemberLifecycle>(kMemberLifecycleTokens, lifecycle);
}

const char* to_string(SiteLifecycle lifecycle) {
  return lookup<SiteLifecycle>(kSiteLifecycleTokens, lifecycle);
}

const char* to_string(DecisionKind kind) {
  return lookup<DecisionKind>(kDecisionKindTokens, kind);
}

bool parse_decision_kind(std::string_view token, DecisionKind& out) {
  return reverse_lookup<DecisionKind>(kDecisionKindTokens, token, out);
}

const char* to_string(AuthorityKind kind) {
  return lookup<AuthorityKind>(kAuthorityKindTokens, kind);
}

bool parse_authority_kind(std::string_view token, AuthorityKind& out) {
  return reverse_lookup<AuthorityKind>(kAuthorityKindTokens, token,
                                                                   out);
}

bool is_valid(AuthorityKind kind) noexcept {
  switch (kind) {
    case AuthorityKind::SITE_CONTROLLER:
    case AuthorityKind::MEMBER_PUBLISHER:
    case AuthorityKind::MAINTENANCE_OPERATOR:
    case AuthorityKind::OBSERVER:
      return true;
    default:
      return false;
  }
}

bool may_mutate_site(AuthorityKind kind) noexcept {
  return kind == AuthorityKind::SITE_CONTROLLER || kind == AuthorityKind::MAINTENANCE_OPERATOR;
}

const char* to_string(LeaseState state) {
  return lookup<LeaseState>(kLeaseStateTokens, state);
}

const char* to_string(RecordType type) {
  return lookup<RecordType>(kRecordTypeTokens, type);
}

bool is_known(RecordType type) noexcept {
  return type != RecordType::UNKNOWN;
}

const char* to_string(MessageType type) {
  return lookup<MessageType>(kMessageTypeTokens, type);
}

bool is_known(MessageType type) noexcept { return type != MessageType::UNKNOWN; }

std::string PublishAttempt::to_string() const {
  return std::to_string(sequence) + "/" + std::to_string(attempt);
}

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
         std::to_string(kVersionPatch);
}

std::string version_tuple() {
  return version_string() + " (wire " + std::to_string(kWireProtocolVersion) + ", store " +
         std::to_string(kPersistenceFormatVersion) + ", model " + std::to_string(kModelSchemaMajor) +
         "." + std::to_string(kModelSchemaMinor) + ")";
}

std::string build_description() {
  std::string out = "Site Fabric " + version_string();
#if defined(NDEBUG)
  out += " Release";
#else
  out += " Debug";
#endif
  out += " | wire " + std::to_string(kWireProtocolVersion);
  out += " | store " + std::to_string(kPersistenceFormatVersion);
  out += " | model " + std::to_string(kModelSchemaMajor) + "." +
         std::to_string(kModelSchemaMinor);
  return out;
}

std::string format_versions() {
  return "wire=" + std::to_string(kWireProtocolVersion) +
         " persistence=" + std::to_string(kPersistenceFormatVersion) +
         " model=" + std::to_string(kModelSchemaMajor) + "." +
         std::to_string(kModelSchemaMinor);
}

}  // namespace site_fabric
