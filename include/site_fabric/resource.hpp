// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Resources a member domain may contribute to a site.
//
// A claim is a statement by one member domain about one resource. Claims are
// never merged by the composer: identical claims from several domains are
// recorded with several attestors and counted once, while differing claims are
// reported as a conflict and excluded from the aggregate. Site Fabric never
// picks a winner on its own.

#ifndef SITE_FABRIC_RESOURCE_HPP
#define SITE_FABRIC_RESOURCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "site_fabric/capacity.hpp"
#include "site_fabric/evidence.hpp"
#include "site_fabric/identity.hpp"

namespace site_fabric {

/// Observed connectivity of a link or gateway.
///
/// UNKNOWN is the default. A resource whose state was never reported is not
/// assumed to be up, and it is not assumed to be down either.
enum class ConnectivityState {
  UNKNOWN = 0,
  UP,
  DOWN,
  DEGRADED,
};

[[nodiscard]] const char* to_string(ConnectivityState state);
[[nodiscard]] bool parse_connectivity_state(std::string_view token, ConnectivityState& out);

/// True only for UP. DEGRADED is not UP.
[[nodiscard]] bool is_operational(ConnectivityState state) noexcept;

/// A member domain's statement about one rack.
///
/// The declaring domain is the owner. A rack claimed by two domains is an
/// ownership overlap, not a shared resource.
struct RackClaim {
  RackId id;
  Generation generation;
  Digest record_digest;
  CapacityVector local_capacity;
  ConnectivityState uplink = ConnectivityState::UNKNOWN;
  Evidence evidence;

  friend bool operator==(const RackClaim&, const RackClaim&) = default;
};

/// A member domain's statement about one pod.
struct PodClaim {
  PodId id;
  Generation generation;
  Digest record_digest;
  std::vector<RackId> racks;
  Evidence evidence;

  friend bool operator==(const PodClaim&, const PodClaim&) = default;
};

/// A member domain's statement about one cluster.
struct ClusterClaim {
  ClusterId id;
  Generation generation;
  Digest record_digest;
  std::vector<PodId> pods;
  std::vector<RackId> racks;
  Evidence evidence;

  friend bool operator==(const ClusterClaim&, const ClusterClaim&) = default;
};

/// A member domain's statement about one shared link.
///
/// Shared links are the reason aggregation cannot be a sum. A link declared by
/// three member domains carries one capacity, not three, and the composer
/// attributes it to the single agreed value while recording all three
/// attestors. Differing capacities are an accounting conflict.
///
/// A link is external when it leaves the site boundary; external links feed
/// the ingress and egress aggregates, internal links feed the internal one.
struct SharedLinkClaim {
  LinkId id;
  bool external = false;
  Generation generation;
  Digest record_digest;
  CapacityVector capacity;
  ConnectivityState state = ConnectivityState::UNKNOWN;
  Evidence evidence;

  friend bool operator==(const SharedLinkClaim&, const SharedLinkClaim&) = default;
};

/// A member domain's statement about one gateway.
struct GatewayClaim {
  GatewayId id;
  Generation generation;
  Digest record_digest;
  CapacityVector capacity;
  ConnectivityState state = ConnectivityState::UNKNOWN;
  Evidence evidence;

  friend bool operator==(const GatewayClaim&, const GatewayClaim&) = default;
};

/// A capacity contribution not attached to a named resource.
///
/// Domains that own a pool of capacity rather than a named link declare it
/// here. The owner key names the pool and is the unit of de-duplication.
struct CapacityContribution {
  ResourceKey owner;
  CapacityScope scope = CapacityScope::UNKNOWN;
  Generation generation;
  Digest record_digest;
  CapacityVector capacity;
  Evidence evidence;

  friend bool operator==(const CapacityContribution&, const CapacityContribution&) = default;
};

/// A publisher's proposed change to a member domain's seal.
///
/// Present on every declaration so that a replayed publication can be
/// distinguished from a fresh one even when the payload is byte-identical.
struct PublishAttempt {
  std::uint64_t sequence = 0;
  std::uint32_t attempt = 0;

  [[nodiscard]] bool valid() const noexcept { return sequence != 0; }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const PublishAttempt&, const PublishAttempt&) = default;
  friend auto operator<=>(const PublishAttempt&, const PublishAttempt&) = default;
};

/// A request to retract a member domain's contribution.
///
/// Retirement is the only way a domain leaves a site without being treated as
/// missing. It is fenced by the same epoch, incarnation and generation rules as
/// any other publication, and it is durable: a retired domain never comes back
/// at the generation it retired from.
struct RetireRequest {
  MemberDomainKey domain;
  Generation generation;
  Incarnation incarnation;
  std::string reason;

  friend bool operator==(const RetireRequest&, const RetireRequest&) = default;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_RESOURCE_HPP
