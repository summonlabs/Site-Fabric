// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The snapshot chain.
//
// Publishing is where a stale controller is stopped. The chain keeps the epoch,
// incarnation and generation of the snapshot it last accepted, and it refuses
// anything that does not strictly advance one of them. Nothing about the
// caller's health, uptime or good intentions enters the decision.

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/digest.hpp"
#include "site_fabric/snapshot.hpp"

namespace site_fabric {

std::string SiteSnapshot::to_string() const {
  std::string out = site.value();
  out += " seq=" + std::to_string(sequence);
  out += " epoch=" + epoch.to_string();
  out += " gen=" + generation.to_string();
  out += " ";
  out += site_fabric::to_string(lifecycle);
  out += complete ? " complete" : " incomplete";
  return out;
}

Status SiteSnapshot::recompute_digest() {
  const Digest state_digest = internal::composed_site_digest(state);
  if (!site_digest.is_zero() && !(state_digest == site_digest)) {
    return Status::INTEGRITY_FAILURE;
  }
  site_digest = state_digest;
  snapshot_digest = internal::snapshot_envelope_digest(*this);
  return Status::OK;
}

std::string SnapshotPublishResult::to_string() const {
  std::string out = site_fabric::to_string(status);
  out += " sequence=" + std::to_string(sequence);
  if (!factors.empty()) {
    out += " [";
    out += factors.join(",");
    out += "]";
  }
  return out;
}

struct SnapshotChain::Impl {
  mutable std::mutex mutex;
  std::deque<SiteSnapshot> history;
  std::uint32_t limit = limits::kDefaultSnapshotHistory;
  std::map<int, std::uint64_t> refusals;
};

SnapshotChain::~SnapshotChain() = default;

SnapshotChain::SnapshotChain(std::uint32_t history_limit) : impl_(std::make_unique<Impl>()) {
  if (history_limit == 0) {
    history_limit = limits::kDefaultSnapshotHistory;
  }
  if (history_limit > limits::kMaxSnapshotHistory) {
    history_limit = limits::kMaxSnapshotHistory;
  }
  impl_->limit = history_limit;
}

Status SnapshotChain::publish(const SiteSnapshot& snapshot, const AuthorityToken& token,
                              std::int64_t now_ms, SnapshotPublishResult& out) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  const auto refuse = [&](Status code, std::string factor) {
    impl_->refusals[static_cast<int>(code)] += 1;
    out.status = code;
    out.sequence = 0;
    out.factors = Factors{};
    if (!factor.empty()) {
      out.factors.add(std::move(factor));
    }
    return code;
  };

  if (!snapshot.site.valid() || !snapshot.epoch.is_set()) {
    return refuse(Status::INVALID, "snapshot_header");
  }
  if (!(token.site == snapshot.site)) {
    return refuse(Status::SITE_MISMATCH, "token_site_differs");
  }
  const Status token_status = token.evaluate_at(now_ms);
  if (!is_ok(token_status)) {
    return refuse(token_status, "token");
  }

  SiteSnapshot candidate = snapshot;
  candidate.sequence = impl_->history.empty() ? 1 : impl_->history.back().sequence + 1;

  if (!(candidate.state.site == candidate.site)) {
    return refuse(Status::INVALID, "state_site_differs");
  }
  if (!(candidate.state.epoch == candidate.epoch) ||
      !(candidate.state.generation == candidate.generation)) {
    return refuse(Status::INVALID, "state_epoch_or_generation_differs");
  }
  const Digest state_digest = internal::composed_site_digest(candidate.state);
  if (!candidate.site_digest.is_zero() && !(state_digest == candidate.site_digest)) {
    return refuse(Status::INTEGRITY_FAILURE, "site_digest");
  }
  candidate.site_digest = state_digest;
  candidate.snapshot_digest = internal::snapshot_envelope_digest(candidate);

  if (impl_->history.empty()) {
    impl_->history.push_back(std::move(candidate));
    out.status = Status::PUBLISHED;
    out.sequence = impl_->history.back().sequence;
    out.factors = Factors{};
    return Status::PUBLISHED;
  }

  const SiteSnapshot& current = impl_->history.back();

  if (token.epoch < current.epoch) {
    return refuse(Status::FENCED_EPOCH, "lower_epoch");
  }
  if (token.epoch == current.epoch) {
    if (!(token.incarnation == current.incarnation)) {
      if (token.incarnation.sequence < current.incarnation.sequence) {
        return refuse(Status::FENCED_INCARNATION, "lower_incarnation_sequence");
      }
    } else {
      if (token.generation < current.generation) {
        return refuse(Status::FENCED_GENERATION, "lower_generation");
      }
      if (token.generation == current.generation) {
        if (candidate.site_digest == current.site_digest) {
          impl_->refusals[static_cast<int>(Status::ALREADY_EXISTS)] += 1;
          out.status = Status::ALREADY_EXISTS;
          out.sequence = current.sequence;
          out.factors = Factors{};
          out.factors.add("republication", "identical");
          return Status::ALREADY_EXISTS;
        }
        return refuse(Status::FENCED_GENERATION, "same_generation_different_content");
      }
    }
  }

  candidate.sequence = current.sequence + 1;
  candidate.snapshot_digest = internal::snapshot_envelope_digest(candidate);
  impl_->history.push_back(std::move(candidate));
  while (impl_->history.size() > impl_->limit) {
    impl_->history.pop_front();
  }

  out.status = Status::PUBLISHED;
  out.sequence = impl_->history.back().sequence;
  out.factors = Factors{};
  return Status::PUBLISHED;
}

Status SnapshotChain::restore(const SiteSnapshot& snapshot) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);

  if (!snapshot.site.valid() || !snapshot.epoch.is_set() || snapshot.sequence == 0) {
    return Status::INVALID;
  }
  const Digest state_digest = internal::composed_site_digest(snapshot.state);
  if (!(state_digest == snapshot.site_digest)) {
    return Status::INTEGRITY_FAILURE;
  }
  const Digest envelope = internal::snapshot_envelope_digest(snapshot);
  if (!snapshot.snapshot_digest.is_zero() && !(envelope == snapshot.snapshot_digest)) {
    return Status::INTEGRITY_FAILURE;
  }

  if (!impl_->history.empty()) {
    const std::uint64_t newest = impl_->history.back().sequence;
    if (snapshot.sequence == newest) {
      return Status::ALREADY_EXISTS;
    }
    if (snapshot.sequence < newest) {
      return Status::REPLAYED;
    }
  }
  impl_->history.push_back(snapshot);
  while (impl_->history.size() > impl_->limit) {
    impl_->history.pop_front();
  }
  return Status::OK;
}

bool SnapshotChain::current(SiteSnapshot& out) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->history.empty()) {
    return false;
  }
  out = impl_->history.back();
  return true;
}

bool SnapshotChain::at_sequence(std::uint64_t sequence, SiteSnapshot& out) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  for (const auto& snapshot : impl_->history) {
    if (snapshot.sequence == sequence) {
      out = snapshot;
      return true;
    }
  }
  return false;
}

std::size_t SnapshotChain::size() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->history.size();
}

void SnapshotChain::set_history_limit(std::uint32_t history_limit) {
  if (history_limit == 0) {
    history_limit = limits::kDefaultSnapshotHistory;
  }
  if (history_limit > limits::kMaxSnapshotHistory) {
    history_limit = limits::kMaxSnapshotHistory;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->limit = history_limit;
}

std::size_t SnapshotChain::history_limit() const { return impl_->limit; }

std::vector<std::uint64_t> SnapshotChain::sequences() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<std::uint64_t> out;
  out.reserve(impl_->history.size());
  for (const auto& snapshot : impl_->history) {
    out.push_back(snapshot.sequence);
  }
  return out;
}

Epoch SnapshotChain::epoch() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->history.empty() ? Epoch::unset() : impl_->history.back().epoch;
}

Incarnation SnapshotChain::incarnation() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->history.empty() ? Incarnation{} : impl_->history.back().incarnation;
}

Generation SnapshotChain::generation() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->history.empty() ? Generation::unset() : impl_->history.back().generation;
}

std::uint64_t SnapshotChain::refusals(Status code) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->refusals.find(static_cast<int>(code));
  return found == impl_->refusals.end() ? 0 : found->second;
}

void SnapshotChain::clear() {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->history.clear();
  impl_->refusals.clear();
}

}  // namespace site_fabric
