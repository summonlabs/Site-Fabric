// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The authority registry.
//
// One mutex guards the whole registry. Every public entry point takes it once,
// mutates a small map, and releases it before returning. No entry point calls
// out to user code, takes a second lock, or performs I/O while holding it, so
// there is no lock-ordering question to get wrong: there is exactly one lock
// and it is a leaf.

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "site_fabric/authority.hpp"

namespace site_fabric {

// ---------------------------------------------------------------------------
// Token
// ---------------------------------------------------------------------------

bool AuthorityToken::structurally_valid() const noexcept {
  if (!is_valid(kind) || holder.empty() || !site.valid()) {
    return false;
  }
  if (!epoch.is_set() || !incarnation.valid()) {
    return false;
  }
  if (lease_id == 0 || issued_at_ms <= 0 || expires_at_ms < issued_at_ms) {
    return false;
  }
  return true;
}

Status AuthorityToken::evaluate_at(std::int64_t now_ms) const {
  if (!structurally_valid()) {
    return Status::INVALID;
  }
  if (now_ms < issued_at_ms) {
    return Status::LEASE_NOT_YET_VALID;
  }
  if (now_ms > expires_at_ms) {
    return Status::LEASE_EXPIRED;
  }
  return Status::OK;
}

std::string AuthorityToken::to_string() const {
  std::string out = site_fabric::to_string(kind);
  out += ":";
  out += holder;
  out += "@";
  out += site.value();
  out += " epoch=" + epoch.to_string();
  out += " incarnation=" + incarnation.to_string();
  out += " gen=" + generation.to_string();
  out += " lease=" + std::to_string(lease_id);
  out += " until=" + std::to_string(expires_at_ms);
  return out;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

namespace {

struct DomainState {
  Incarnation live;
  bool has_live = false;
  std::uint64_t next_sequence = 1;
  Epoch observed_epoch;
  Generation generation;
  Digest digest;
  PublishAttempt attempt;
  bool published = false;
  std::int64_t updated_at_ms = 0;
};

[[nodiscard]] std::string lease_key(const SiteId& site, AuthorityKind kind,
                                    const std::string& holder) {
  std::string key = site.value();
  key.push_back('/');
  key += site_fabric::to_string(kind);
  key.push_back('/');
  key += holder;
  return key;
}

}  // namespace

struct AuthorityRegistry::Impl {
  mutable std::mutex mutex;
  std::map<std::string, LeaseRecord> leases;
  std::map<SiteId, Epoch> site_epochs;
  std::map<SiteId, std::string> site_controllers;
  std::map<MemberDomainKey, DomainState> domains;
  std::map<MemberDomainKey, std::set<std::string>> tombstones;
  std::uint64_t next_lease_id = 1;
};

AuthorityRegistry::AuthorityRegistry() : impl_(std::make_unique<Impl>()) {}
AuthorityRegistry::~AuthorityRegistry() = default;

Status AuthorityRegistry::acquire_site_authority(const std::string& holder, const SiteId& site,
                                                 Epoch epoch, const Incarnation& incarnation,
                                                 Generation generation, std::int64_t now_ms,
                                                 std::int64_t ttl_ms, AuthorityToken& out) {
  if (holder.empty() || !site.valid() || !epoch.is_set() || !incarnation.valid() ||
      ttl_ms <= 0 || now_ms <= 0) {
    return Status::INVALID;
  }

  const std::lock_guard<std::mutex> guard(impl_->mutex);

  Epoch& known = impl_->site_epochs[site];
  if (known.is_set() && epoch < known) {
    return Status::FENCED_EPOCH;
  }
  if (epoch > known) {
    known = epoch;
  }

  const std::string key = lease_key(site, AuthorityKind::SITE_CONTROLLER, holder);
  const auto existing = impl_->leases.find(key);

  if (existing == impl_->leases.end()) {
    // A different holder may already control this site in this epoch.
    const auto controller = impl_->site_controllers.find(site);
    if (controller != impl_->site_controllers.end() && controller->second != key) {
      const auto other = impl_->leases.find(controller->second);
      if (other != impl_->leases.end() && other->second.state == LeaseState::ACTIVE &&
          other->second.token.expires_at_ms >= now_ms) {
        return Status::AUTHORITY_HELD_ELSEWHERE;
      }
      if (other != impl_->leases.end() && other->second.state == LeaseState::REVOKED) {
        return Status::LEASE_REVOKED;
      }
      if (other != impl_->leases.end() &&
          other->second.token.incarnation.sequence >= incarnation.sequence) {
        return Status::FENCED_INCARNATION;
      }
      // The previous controller's lease lapsed without revocation; take over.
      if (other != impl_->leases.end()) {
        other->second.state = LeaseState::SUPERSEDED;
        other->second.updated_at_ms = now_ms;
        other->second.note = "superseded by " + holder;
      }
    }

    AuthorityToken token;
    token.kind = AuthorityKind::SITE_CONTROLLER;
    token.holder = holder;
    token.site = site;
    token.epoch = epoch;
    token.incarnation = incarnation;
    token.generation = generation;
    token.lease_id = impl_->next_lease_id++;
    token.issued_at_ms = now_ms;
    token.expires_at_ms = now_ms + ttl_ms;

    LeaseRecord record;
    record.token = token;
    record.state = LeaseState::ACTIVE;
    record.updated_at_ms = now_ms;
    impl_->leases[key] = record;
    impl_->site_controllers[site] = key;
    out = token;
    return Status::GRANTED;
  }

  LeaseRecord& record = existing->second;
  if (record.state == LeaseState::REVOKED) {
    return Status::LEASE_REVOKED;
  }
  if (!(record.token.incarnation == incarnation)) {
    if (record.state == LeaseState::ACTIVE && record.token.expires_at_ms >= now_ms) {
      return Status::AUTHORITY_HELD_ELSEWHERE;
    }
    if (record.token.incarnation.sequence >= incarnation.sequence) {
      return Status::FENCED_INCARNATION;
    }
    record.state = LeaseState::SUPERSEDED;
    record.updated_at_ms = now_ms;
    record.note = "superseded in place";
  }
  if (generation < record.token.generation) {
    return Status::FENCED_GENERATION;
  }

  record.token.epoch = epoch;
  record.token.incarnation = incarnation;
  record.token.generation = generation;
  record.token.issued_at_ms = now_ms;
  record.token.expires_at_ms = now_ms + ttl_ms;
  record.state = LeaseState::ACTIVE;
  record.updated_at_ms = now_ms;
  impl_->site_controllers[site] = key;
  out = record.token;
  return Status::GRANTED;
}

Status AuthorityRegistry::renew(const AuthorityToken& token, std::int64_t now_ms,
                                std::int64_t ttl_ms, AuthorityToken& out) {
  if (!token.structurally_valid() || ttl_ms <= 0 || now_ms <= 0) {
    return Status::INVALID;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  const auto site_epoch = impl_->site_epochs.find(token.site);
  if (site_epoch != impl_->site_epochs.end() && token.epoch < site_epoch->second) {
    return Status::FENCED_EPOCH;
  }

  const std::string key = lease_key(token.site, token.kind, token.holder);
  const auto existing = impl_->leases.find(key);
  if (existing == impl_->leases.end()) {
    return Status::UNAUTHORIZED;
  }
  LeaseRecord& record = existing->second;
  if (record.token.lease_id != token.lease_id) {
    return Status::UNAUTHORIZED;
  }
  if (!(record.token.incarnation == token.incarnation)) {
    return Status::FENCED_INCARNATION;
  }
  if (!(record.token.generation == token.generation)) {
    return Status::FENCED_GENERATION;
  }
  if (record.state == LeaseState::REVOKED) {
    return Status::LEASE_REVOKED;
  }
  const Status evaluated = record.token.evaluate_at(now_ms);
  if (evaluated == Status::LEASE_EXPIRED) {
    record.state = LeaseState::EXPIRED;
    record.updated_at_ms = now_ms;
    return Status::LEASE_EXPIRED;
  }
  if (!is_ok(evaluated)) {
    return evaluated;
  }

  record.token.expires_at_ms = now_ms + ttl_ms;
  record.token.issued_at_ms = now_ms;
  record.state = LeaseState::ACTIVE;
  record.updated_at_ms = now_ms;
  record.renewals += 1;
  out = record.token;
  return Status::RENEWED;
}

Status AuthorityRegistry::revoke(const AuthorityToken& token, std::int64_t now_ms,
                                 std::string note) {
  if (!token.structurally_valid()) {
    return Status::INVALID;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const std::string key = lease_key(token.site, token.kind, token.holder);
  const auto existing = impl_->leases.find(key);
  if (existing == impl_->leases.end()) {
    return Status::NOT_FOUND;
  }
  LeaseRecord& record = existing->second;
  if (record.token.lease_id != token.lease_id) {
    return Status::UNAUTHORIZED;
  }
  record.state = LeaseState::REVOKED;
  record.updated_at_ms = now_ms;
  record.note = std::move(note);
  return Status::OK;
}

Status AuthorityRegistry::validate(const AuthorityToken& token, std::int64_t now_ms) const {
  if (!token.structurally_valid()) {
    return Status::INVALID;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  const auto site_epoch = impl_->site_epochs.find(token.site);
  if (site_epoch != impl_->site_epochs.end()) {
    if (token.epoch < site_epoch->second) {
      return Status::FENCED_EPOCH;
    }
    if (token.epoch > site_epoch->second) {
      return Status::UNAUTHORIZED;
    }
  } else {
    return Status::UNAUTHORIZED;
  }

  const std::string key = lease_key(token.site, token.kind, token.holder);
  const auto existing = impl_->leases.find(key);
  if (existing == impl_->leases.end()) {
    return Status::UNAUTHORIZED;
  }
  const LeaseRecord& record = existing->second;
  if (record.token.lease_id != token.lease_id) {
    return Status::UNAUTHORIZED;
  }
  if (!(record.token.incarnation == token.incarnation)) {
    return Status::FENCED_INCARNATION;
  }
  if (token.generation < record.token.generation) {
    return Status::FENCED_GENERATION;
  }
  if (record.state == LeaseState::REVOKED) {
    return Status::LEASE_REVOKED;
  }
  if (record.state == LeaseState::SUPERSEDED) {
    return Status::AUTHORITY_HELD_ELSEWHERE;
  }
  return record.token.evaluate_at(now_ms);
}

Epoch AuthorityRegistry::current_epoch(const SiteId& site) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->site_epochs.find(site);
  return found == impl_->site_epochs.end() ? Epoch::unset() : found->second;
}

bool AuthorityRegistry::current_controller(const SiteId& site, AuthorityToken& out) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto controller = impl_->site_controllers.find(site);
  if (controller == impl_->site_controllers.end()) {
    return false;
  }
  const auto record = impl_->leases.find(controller->second);
  if (record == impl_->leases.end()) {
    return false;
  }
  out = record->second.token;
  return true;
}

Status AuthorityRegistry::admit_member_incarnation(const MemberDomainKey& domain,
                                                   const Incarnation& incarnation,
                                                   std::int64_t now_ms,
                                                   std::uint64_t* out_sequence) {
  if (!domain.valid() || !incarnation.boot.valid() || now_ms <= 0) {
    return Status::INVALID;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  DomainState& state = impl_->domains[domain];

  // A caller with no sequence is asking to be issued one. The issued value is
  // the domain's next sequence, which only ever moves forward, so a process
  // cannot pick a sequence to outrank another.
  Incarnation candidate = incarnation;
  if (candidate.sequence == 0) {
    candidate.sequence = state.next_sequence;
  }

  const auto tombstoned = impl_->tombstones.find(domain);
  if (tombstoned != impl_->tombstones.end() &&
      tombstoned->second.count(candidate.to_string()) != 0) {
    return Status::FENCED_INCARNATION;
  }

  if (!state.has_live) {
    if (candidate.sequence < state.next_sequence) {
      return Status::FENCED_INCARNATION;
    }
    state.live = candidate;
    state.has_live = true;
    state.next_sequence = candidate.sequence + 1;
    state.updated_at_ms = now_ms;
    state.published = false;
    state.generation = Generation::unset();
    state.digest = Digest::zero();
    state.attempt = PublishAttempt{};
    if (out_sequence != nullptr) {
      *out_sequence = candidate.sequence;
    }
    return Status::OK;
  }

  if (state.live == candidate) {
    if (out_sequence != nullptr) {
      *out_sequence = candidate.sequence;
    }
    return Status::ALREADY_EXISTS;
  }
  if (candidate.sequence < state.live.sequence) {
    return Status::FENCED_INCARNATION;
  }
  if (candidate.sequence == state.live.sequence) {
    // Same sequence, different process. A sequence is issued, never guessed.
    return Status::FENCED_INCARNATION;
  }

  impl_->tombstones[domain].insert(state.live.to_string());
  state.live = candidate;
  state.next_sequence = candidate.sequence + 1;
  state.updated_at_ms = now_ms;
  state.published = false;
  state.generation = Generation::unset();
  state.digest = Digest::zero();
  state.attempt = PublishAttempt{};
  if (out_sequence != nullptr) {
    *out_sequence = candidate.sequence;
  }
  return Status::OK;
}

Status AuthorityRegistry::retire_member_incarnation(const MemberDomainKey& domain,
                                                    const Incarnation& incarnation,
                                                    std::int64_t now_ms, std::string reason) {
  if (!domain.valid() || !incarnation.valid()) {
    return Status::INVALID;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->tombstones[domain].insert(incarnation.to_string());
  const auto found = impl_->domains.find(domain);
  if (found != impl_->domains.end() && found->second.has_live &&
      found->second.live == incarnation) {
    found->second.has_live = false;
    found->second.updated_at_ms = now_ms;
  }
  (void)reason;
  return Status::OK;
}

bool AuthorityRegistry::member_incarnation_live(const MemberDomainKey& domain,
                                                const Incarnation& incarnation) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto tombstoned = impl_->tombstones.find(domain);
  if (tombstoned != impl_->tombstones.end() &&
      tombstoned->second.count(incarnation.to_string()) != 0) {
    return false;
  }
  const auto found = impl_->domains.find(domain);
  return found != impl_->domains.end() && found->second.has_live &&
         found->second.live == incarnation;
}

std::uint64_t AuthorityRegistry::next_member_sequence(const MemberDomainKey& domain) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->domains.find(domain);
  return found == impl_->domains.end() ? 1 : found->second.next_sequence;
}

Status AuthorityRegistry::fence_publication(const MemberDomainKey& domain, Epoch epoch,
                                            const Incarnation& incarnation,
                                            Generation generation, const Digest& digest,
                                            const PublishAttempt& attempt,
                                            std::int64_t now_ms) const {
  if (!domain.valid() || !generation.is_set() || digest.is_zero() || !attempt.valid()) {
    return Status::INVALID;
  }
  (void)now_ms;
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  const auto tombstoned = impl_->tombstones.find(domain);
  if (tombstoned != impl_->tombstones.end() &&
      tombstoned->second.count(incarnation.to_string()) != 0) {
    return Status::FENCED_INCARNATION;
  }

  const auto found = impl_->domains.find(domain);
  if (found == impl_->domains.end() || !found->second.has_live) {
    return Status::UNAUTHORIZED;
  }
  const DomainState& state = found->second;
  if (!(state.live == incarnation)) {
    return Status::FENCED_INCARNATION;
  }
  if (state.observed_epoch.is_set() && epoch < state.observed_epoch) {
    return Status::FENCED_EPOCH;
  }
  if (state.published) {
    if (generation < state.generation) {
      return Status::FENCED_GENERATION;
    }
    if (generation == state.generation) {
      if (!(digest == state.digest)) {
        // A generation is immutable. New content needs a new generation.
        return Status::FENCED_GENERATION;
      }
      if (attempt.sequence < state.attempt.sequence) {
        return Status::REPLAYED;
      }
      if (attempt.sequence == state.attempt.sequence &&
          attempt.attempt <= state.attempt.attempt) {
        return Status::FENCED_ATTEMPT;
      }
    }
    if (attempt.sequence != 0 && attempt.sequence < state.attempt.sequence) {
      return Status::REPLAYED;
    }
  }
  return Status::OK;
}

Status AuthorityRegistry::record_publication(const MemberDomainKey& domain, Epoch epoch,
                                             Generation generation, const Digest& digest,
                                             const PublishAttempt& attempt,
                                             std::int64_t now_ms) {
  if (!domain.valid() || !generation.is_set() || digest.is_zero()) {
    return Status::INVALID;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  DomainState& state = impl_->domains[domain];
  state.observed_epoch = epoch;
  state.generation = generation;
  state.digest = digest;
  state.attempt = attempt;
  state.published = true;
  state.updated_at_ms = now_ms;
  return Status::OK;
}

void AuthorityRegistry::clear_domain(const MemberDomainKey& domain) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->domains.erase(domain);
}

std::vector<LeaseRecord> AuthorityRegistry::leases() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<LeaseRecord> out;
  out.reserve(impl_->leases.size());
  for (const auto& [key, record] : impl_->leases) {
    (void)key;
    out.push_back(record);
  }
  std::sort(out.begin(), out.end(), [](const LeaseRecord& left, const LeaseRecord& right) {
    return left.token.to_string() < right.token.to_string();
  });
  return out;
}

std::vector<std::string> AuthorityRegistry::tombstones() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<std::string> out;
  for (const auto& [domain, set] : impl_->tombstones) {
    for (const auto& incarnation : set) {
      out.push_back(domain.to_string() + "/" + incarnation);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t AuthorityRegistry::domain_count() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->domains.size();
}

std::string AuthorityRegistry::to_string() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::size_t tombstone_count = 0;
  for (const auto& [domain, set] : impl_->tombstones) {
    (void)domain;
    tombstone_count += set.size();
  }
  return "leases=" + std::to_string(impl_->leases.size()) +
         " sites=" + std::to_string(impl_->site_epochs.size()) +
         " domains=" + std::to_string(impl_->domains.size()) +
         " tombstones=" + std::to_string(tombstone_count);
}

}  // namespace site_fabric
