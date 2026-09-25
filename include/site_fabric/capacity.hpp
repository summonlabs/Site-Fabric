// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Capacity values and checked aggregation.
//
// A capacity channel is either known or unknown. There is no third state and
// there is no default of zero: an unreported channel is UNKNOWN, and an
// aggregate that includes an unknown channel is INDETERMINATE rather than
// smaller-but-fine.
//
// Every addition is checked. A sum that would exceed the plausibility bound or
// wrap the accumulator is reported as CAPACITY_OVERFLOW and the accumulator is left
// untouched.

#ifndef SITE_FABRIC_CAPACITY_HPP
#define SITE_FABRIC_CAPACITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "site_fabric/status.hpp"

namespace site_fabric {

/// One capacity channel: known value, or unknown.
struct CapacityValue {
  bool known = false;
  std::uint64_t bps = 0;

  constexpr CapacityValue() = default;
  constexpr explicit CapacityValue(std::uint64_t value) : known(true), bps(value) {}

  [[nodiscard]] static constexpr CapacityValue unknown() { return CapacityValue(); }
  [[nodiscard]] static constexpr CapacityValue known_bps(std::uint64_t value) {
    return CapacityValue(value);
  }

  [[nodiscard]] constexpr bool is_known() const noexcept { return known; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return known && bps == 0; }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const CapacityValue&, const CapacityValue&) = default;
};

/// The three channels a site reports.
///
/// ingress:  capacity into the site from outside.
/// egress:   capacity out of the site.
/// internal: capacity available for traffic that stays inside the site.
struct CapacityVector {
  CapacityValue ingress;
  CapacityValue egress;
  CapacityValue internal;

  constexpr CapacityVector() = default;
  constexpr CapacityVector(CapacityValue ingress_value, CapacityValue egress_value,
                           CapacityValue internal_value)
      : ingress(ingress_value), egress(egress_value), internal(internal_value) {}

  [[nodiscard]] static constexpr CapacityVector all_unknown() { return CapacityVector(); }

  [[nodiscard]] static constexpr CapacityVector uniform(std::uint64_t value) {
    return CapacityVector(CapacityValue(value), CapacityValue(value), CapacityValue(value));
  }

  [[nodiscard]] bool all_known() const noexcept {
    return ingress.known && egress.known && internal.known;
  }
  /// True when no channel was reported at all. Distinct from the all_unknown()
  /// factory, which produces the empty vector this predicate recognises.
  [[nodiscard]] bool has_no_reported_channel() const noexcept {
    return !ingress.known && !egress.known && !internal.known;
  }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const CapacityVector&, const CapacityVector&) = default;
};

/// Named channel, used by per-channel obligation checks and by the ledger.
enum class CapacityChannel {
  UNKNOWN = 0,
  INGRESS,
  EGRESS,
  INTERNAL,
};

[[nodiscard]] const char* to_string(CapacityChannel channel);
[[nodiscard]] bool parse_capacity_channel(std::string_view token, CapacityChannel& out);

[[nodiscard]] CapacityValue channel_value(const CapacityVector& vector,
                                          CapacityChannel channel);

/// Where a capacity contribution sits in the site.
///
/// The scope decides which aggregate a contribution feeds. A rack-local
/// contribution feeds the internal aggregate only; a site-ingress
/// contribution feeds the ingress aggregate only.
enum class CapacityScope {
  UNKNOWN = 0,
  SITE_INGRESS,
  SITE_EGRESS,
  SITE_INTERNAL,
  RACK_LOCAL,
  POD_LOCAL,
  CLUSTER_LOCAL,
  SHARED_LINK,
  GATEWAY,
};

[[nodiscard]] const char* to_string(CapacityScope scope);
[[nodiscard]] bool parse_capacity_scope(std::string_view token, CapacityScope& out);
[[nodiscard]] bool is_valid(CapacityScope scope) noexcept;

/// True when a scope with this kind of owner contributes to the site total.
[[nodiscard]] bool scope_feeds_site_total(CapacityScope scope) noexcept;

/// Checked addition of two concrete vectors.
///
/// Returns Status::CAPACITY_OVERFLOW and leaves the accumulator untouched when the sum
/// would exceed limits::kMaxCapacityBps in any known channel. Adding an
/// unknown channel to a known one yields unknown for that channel: unknown is
/// absorbing, so a hole can never be filled by a later contribution.
Status add_capacity(CapacityVector& accumulator, const CapacityVector& addend);

/// An accumulator that distinguishes "nothing contributed yet" from
/// "contributed, value unknown".
///
/// This distinction is the whole reason capacity aggregation cannot be a plain
/// sum. An empty contribution set aggregates to UNKNOWN, not to zero, because
/// a site that reported nothing has not reported an empty site. A contribution
/// whose channel is unknown makes the aggregate channel unknown for good: no
/// later contribution can fill the hole.
class CapacityAggregate {
 public:
  CapacityAggregate() = default;

  /// Adds one contribution. Returns Status::CAPACITY_OVERFLOW when this contribution
  /// would push a channel past the plausibility bound; the channel is then
  /// permanently unknown and the aggregate records that it overflowed.
  Status add(const CapacityVector& contribution);

  /// Adds one value to one channel. This is the primitive the ledger uses,
  /// because a contribution usually feeds some channels and not others:
  /// a rack-local pool feeds internal capacity and says nothing at all about
  /// ingress, which must stay unreported rather than become zero.
  ///
  /// Does not move the contribution counter; call count_entry() once per
  /// contribution.
  Status add_channel(CapacityChannel channel, const CapacityValue& value);

  /// Records that one contribution was accounted for, without adding to any
  /// channel. Used when a contribution feeds no channel at all and when a
  /// contribution is added channel by channel.
  void count_entry() noexcept { ++contributions_; }

  /// Adds up to the bound given, stopping at it. Returns the number added.
  Status add_all(const std::vector<CapacityVector>& contributions, std::size_t bound);

  [[nodiscard]] std::size_t contributions() const noexcept { return contributions_; }
  [[nodiscard]] bool empty() const noexcept { return contributions_ == 0; }

  /// Per channel: known only when at least one contribution arrived and every
  /// contribution reported it and no addition overflowed.
  [[nodiscard]] CapacityVector total() const;

  /// OK when every channel is known, CAPACITY_UNKNOWN when a channel has no
  /// contributions or an unknown one, CAPACITY_OVERFLOW when a channel overflowed.
  [[nodiscard]] Status status() const;

  [[nodiscard]] bool overflowed() const noexcept {
    return ingress_overflow_ || egress_overflow_ || internal_overflow_;
  }
  [[nodiscard]] bool channel_known(CapacityChannel channel) const noexcept;

  /// True when at least one contribution mentioned this channel. A channel that
  /// was never mentioned is unreported, which is not the same as zero.
  [[nodiscard]] bool channel_seen(CapacityChannel channel) const noexcept;

 private:
  std::uint64_t ingress_ = 0;
  std::uint64_t egress_ = 0;
  std::uint64_t internal_ = 0;
  std::size_t contributions_ = 0;
  bool ingress_seen_ = false;
  bool egress_seen_ = false;
  bool internal_seen_ = false;
  bool ingress_unknown_ = false;
  bool egress_unknown_ = false;
  bool internal_unknown_ = false;
  bool ingress_overflow_ = false;
  bool egress_overflow_ = false;
  bool internal_overflow_ = false;
};

/// True when every known channel of the part is at most the whole.
///
/// This is the monotonicity check the capacity ledger closes with: available
/// plus excluded must never exceed total, and an unknown whole channel admits
/// any part.
[[nodiscard]] bool capacity_leq(const CapacityVector& part, const CapacityVector& whole) noexcept;

/// Subtracts channel-wise for known channels, leaving a channel unknown when
/// either side is unknown. Returns Status::CAPACITY_UNDERFLOW when a known channel of
/// the subtrahend exceeds the corresponding channel of the minuend.
Status subtract_capacity(const CapacityVector& minuend, const CapacityVector& subtrahend,
                         CapacityVector& out);

/// Maximum of two vectors, channel-wise, treating unknown as absent.
[[nodiscard]] CapacityVector capacity_max(const CapacityVector& left,
                                          const CapacityVector& right) noexcept;

}  // namespace site_fabric

#endif  // SITE_FABRIC_CAPACITY_HPP
