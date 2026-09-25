// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "site_fabric/capacity.hpp"

#include <string>

#include "site_fabric/limits.hpp"

namespace site_fabric {
namespace {

/// Adds one known value into one known accumulator with both bounds checked.
/// Returns CAPACITY_OVERFLOW without touching the accumulator.
[[nodiscard]] Status add_known(std::uint64_t& accumulator, std::uint64_t addend) {
  if (addend > limits::kMaxCapacityBps || accumulator > limits::kMaxCapacityBps) {
    return Status::CAPACITY_OVERFLOW;
  }
  const std::uint64_t sum = accumulator + addend;
  if (sum < accumulator || sum > limits::kMaxCapacityBps) {
    return Status::CAPACITY_OVERFLOW;
  }
  accumulator = sum;
  return Status::OK;
}

}  // namespace

std::string CapacityValue::to_string() const {
  return known ? std::to_string(bps) : std::string("UNKNOWN");
}

std::string CapacityVector::to_string() const {
  return "ingress=" + ingress.to_string() + " egress=" + egress.to_string() +
         " internal=" + internal.to_string();
}

CapacityValue channel_value(const CapacityVector& vector, CapacityChannel channel) {
  switch (channel) {
    case CapacityChannel::INGRESS:
      return vector.ingress;
    case CapacityChannel::EGRESS:
      return vector.egress;
    case CapacityChannel::INTERNAL:
      return vector.internal;
    default:
      return CapacityValue::unknown();
  }
}

Status add_capacity(CapacityVector& accumulator, const CapacityVector& addend) {
  CapacityVector candidate = accumulator;

  if (!addend.ingress.known || !candidate.ingress.known) {
    candidate.ingress = CapacityValue::unknown();
  } else {
    const Status added = add_known(candidate.ingress.bps, addend.ingress.bps);
    if (!is_ok(added)) {
      return added;
    }
  }

  if (!addend.egress.known || !candidate.egress.known) {
    candidate.egress = CapacityValue::unknown();
  } else {
    const Status added = add_known(candidate.egress.bps, addend.egress.bps);
    if (!is_ok(added)) {
      return added;
    }
  }

  if (!addend.internal.known || !candidate.internal.known) {
    candidate.internal = CapacityValue::unknown();
  } else {
    const Status added = add_known(candidate.internal.bps, addend.internal.bps);
    if (!is_ok(added)) {
      return added;
    }
  }

  accumulator = candidate;
  return Status::OK;
}

bool capacity_leq(const CapacityVector& part, const CapacityVector& whole) noexcept {
  if (part.ingress.known && whole.ingress.known && part.ingress.bps > whole.ingress.bps) {
    return false;
  }
  if (part.egress.known && whole.egress.known && part.egress.bps > whole.egress.bps) {
    return false;
  }
  if (part.internal.known && whole.internal.known && part.internal.bps > whole.internal.bps) {
    return false;
  }
  return true;
}

Status subtract_capacity(const CapacityVector& minuend, const CapacityVector& subtrahend,
                         CapacityVector& out) {
  CapacityVector candidate;
  const auto subtract_channel = [](const CapacityValue& left, const CapacityValue& right,
                                   CapacityValue& result) -> Status {
    if (!left.known || !right.known) {
      result = CapacityValue::unknown();
      return Status::OK;
    }
    if (right.bps > left.bps) {
      return Status::CAPACITY_UNDERFLOW;
    }
    result = CapacityValue(left.bps - right.bps);
    return Status::OK;
  };

  Status status = subtract_channel(minuend.ingress, subtrahend.ingress, candidate.ingress);
  if (!is_ok(status)) {
    return status;
  }
  status = subtract_channel(minuend.egress, subtrahend.egress, candidate.egress);
  if (!is_ok(status)) {
    return status;
  }
  status = subtract_channel(minuend.internal, subtrahend.internal, candidate.internal);
  if (!is_ok(status)) {
    return status;
  }
  out = candidate;
  return Status::OK;
}

CapacityVector capacity_max(const CapacityVector& left, const CapacityVector& right) noexcept {
  const auto pick = [](const CapacityValue& a, const CapacityValue& b) {
    if (!a.known) {
      return b;
    }
    if (!b.known) {
      return a;
    }
    return CapacityValue(a.bps > b.bps ? a.bps : b.bps);
  };
  return CapacityVector(pick(left.ingress, right.ingress), pick(left.egress, right.egress),
                        pick(left.internal, right.internal));
}

Status CapacityAggregate::add_channel(CapacityChannel channel, const CapacityValue& value) {
  bool* seen = nullptr;
  bool* unknown = nullptr;
  bool* overflowed = nullptr;
  std::uint64_t* accumulator = nullptr;

  switch (channel) {
    case CapacityChannel::INGRESS:
      seen = &ingress_seen_;
      unknown = &ingress_unknown_;
      overflowed = &ingress_overflow_;
      accumulator = &ingress_;
      break;
    case CapacityChannel::EGRESS:
      seen = &egress_seen_;
      unknown = &egress_unknown_;
      overflowed = &egress_overflow_;
      accumulator = &egress_;
      break;
    case CapacityChannel::INTERNAL:
      seen = &internal_seen_;
      unknown = &internal_unknown_;
      overflowed = &internal_overflow_;
      accumulator = &internal_;
      break;
    default:
      return Status::INVALID;
  }

  *seen = true;
  if (!value.known) {
    *unknown = true;
    return Status::OK;
  }
  if (*overflowed) {
    return Status::OK;
  }
  const Status added = add_known(*accumulator, value.bps);
  if (!is_ok(added)) {
    *overflowed = true;
    return Status::CAPACITY_OVERFLOW;
  }
  return Status::OK;
}

Status CapacityAggregate::add(const CapacityVector& contribution) {
  count_entry();
  Status worst = add_channel(CapacityChannel::INGRESS, contribution.ingress);
  const Status egress_status = add_channel(CapacityChannel::EGRESS, contribution.egress);
  const Status internal_status = add_channel(CapacityChannel::INTERNAL, contribution.internal);
  if (!is_ok(egress_status) && is_ok(worst)) {
    worst = egress_status;
  }
  if (!is_ok(internal_status) && is_ok(worst)) {
    worst = internal_status;
  }
  return worst;
}

Status CapacityAggregate::add_all(const std::vector<CapacityVector>& contributions,
                                  std::size_t bound) {
  Status worst = Status::OK;
  std::size_t added = 0;
  for (const auto& contribution : contributions) {
    if (added >= bound) {
      return Status::LIMIT_EXCEEDED;
    }
    const Status status = add(contribution);
    if (!is_ok(status)) {
      worst = status;
    }
    ++added;
  }
  return worst;
}

bool CapacityAggregate::channel_known(CapacityChannel channel) const noexcept {
  switch (channel) {
    case CapacityChannel::INGRESS:
      return ingress_seen_ && !ingress_unknown_ && !ingress_overflow_;
    case CapacityChannel::EGRESS:
      return egress_seen_ && !egress_unknown_ && !egress_overflow_;
    case CapacityChannel::INTERNAL:
      return internal_seen_ && !internal_unknown_ && !internal_overflow_;
    default:
      return false;
  }
}

bool CapacityAggregate::channel_seen(CapacityChannel channel) const noexcept {
  switch (channel) {
    case CapacityChannel::INGRESS:
      return ingress_seen_;
    case CapacityChannel::EGRESS:
      return egress_seen_;
    case CapacityChannel::INTERNAL:
      return internal_seen_;
    default:
      return false;
  }
}

CapacityVector CapacityAggregate::total() const {
  CapacityVector out;
  if (channel_known(CapacityChannel::INGRESS)) {
    out.ingress = CapacityValue(ingress_);
  }
  if (channel_known(CapacityChannel::EGRESS)) {
    out.egress = CapacityValue(egress_);
  }
  if (channel_known(CapacityChannel::INTERNAL)) {
    out.internal = CapacityValue(internal_);
  }
  return out;
}

Status CapacityAggregate::status() const {
  if (overflowed()) {
    return Status::CAPACITY_OVERFLOW;
  }
  if (contributions_ == 0) {
    return Status::CAPACITY_UNKNOWN;
  }
  if (!channel_known(CapacityChannel::INGRESS) || !channel_known(CapacityChannel::EGRESS) ||
      !channel_known(CapacityChannel::INTERNAL)) {
    return Status::CAPACITY_UNKNOWN;
  }
  return Status::OK;
}

}  // namespace site_fabric
