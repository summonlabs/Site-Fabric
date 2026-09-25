// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Maintenance state and protected obligations.
//
// Maintenance removes capacity from the available aggregate. It never removes
// capacity from the total: the hardware has not gone away, it is simply not
// for sale right now. Reporting a smaller total during a maintenance window
// would quietly destroy the record of what the site owns.
//
// A protected obligation is the opposite direction: a floor the site promised
// to keep. Maintenance is not allowed to breach one, and a maintenance zone
// whose scope overlaps a protected obligation is refused rather than applied.

#ifndef SITE_FABRIC_MAINTENANCE_HPP
#define SITE_FABRIC_MAINTENANCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "site_fabric/capacity.hpp"
#include "site_fabric/evidence.hpp"
#include "site_fabric/failure_domain.hpp"
#include "site_fabric/identity.hpp"

namespace site_fabric {

/// The lifecycle of a maintenance zone.
///
/// SCHEDULED is a statement about the future, ACTIVE is a statement about now,
/// COMPLETE and CANCELLED are terminal. UNKNOWN is not schedulable: a zone
/// whose state was never reported excludes nothing and blocks nothing, but it
/// does make its scope INDETERMINATE.
enum class MaintenanceState {
  UNKNOWN = 0,
  SCHEDULED,
  ACTIVE,
  COMPLETE,
  CANCELLED,
};

[[nodiscard]] const char* to_string(MaintenanceState state);
[[nodiscard]] bool parse_maintenance_state(std::string_view token, MaintenanceState& out);

/// True for states that remove capacity from the available aggregate.
[[nodiscard]] bool excludes_capacity(MaintenanceState state) noexcept;

/// True for states that make the scope indeterminate because the operator's
/// intent is not known.
[[nodiscard]] bool makes_indeterminate(MaintenanceState state) noexcept;

/// A maintenance zone: a named window over a set of domains and resources.
///
/// The scope is the union of the named failure domains (with their nested
/// descendants) and the named resources. Both lists may be empty; an empty
/// scope is INVALID, because a zone that excludes nothing is a no-op that
/// would silently succeed.
struct MaintenanceZone {
  MaintenanceZoneId id;
  MaintenanceState state = MaintenanceState::UNKNOWN;
  std::int64_t window_start_ms = 0;
  std::int64_t window_end_ms = 0;
  std::vector<FailureDomainId> domains;
  std::vector<ResourceKey> resources;
  Generation generation;
  Digest record_digest;
  Evidence evidence;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool window_contains(std::int64_t now_ms) const noexcept;

  friend bool operator==(const MaintenanceZone&, const MaintenanceZone&) = default;
};

/// What a maintenance zone actually did to the composition.
struct MaintenanceEffect {
  MaintenanceZoneId id;
  MaintenanceState state = MaintenanceState::UNKNOWN;
  /// Resources the zone removed from the available aggregate, canonicalised.
  std::vector<ResourceKey> excluded;
  /// Resources the zone made indeterminate.
  std::vector<ResourceKey> indeterminate;
  /// Protected obligations this zone's scope overlaps.
  std::vector<ObligationId> overlapping_obligations;
  CapacityVector excluded_capacity;
  Status status = Status::UNKNOWN;
  Factors factors;

  [[nodiscard]] std::string to_string() const;
};

/// A floor the site promised to keep.
enum class ObligationKind {
  UNKNOWN = 0,
  MIN_INGRESS_CAPACITY,
  MIN_EGRESS_CAPACITY,
  MIN_INTERNAL_CAPACITY,
  MIN_DISTINCT_FAILURE_DOMAINS,
  MIN_UP_SHARED_LINKS,
  MIN_UP_GATEWAYS,
  DOMAIN_SURVIVABILITY,
};

[[nodiscard]] const char* to_string(ObligationKind kind);
[[nodiscard]] bool parse_obligation_kind(std::string_view token, ObligationKind& out);
[[nodiscard]] bool is_valid(ObligationKind kind) noexcept;

/// True when the obligation is counted in resources rather than bits per
/// second; the required value is then a count and is bounded by
/// limits::kMaxCountValue instead of limits::kMaxCapacityBps.
[[nodiscard]] bool obligation_is_count(ObligationKind kind) noexcept;

/// The channel an obligation measures, for the capacity-valued kinds.
[[nodiscard]] CapacityChannel obligation_channel(ObligationKind kind) noexcept;

/// A protected obligation.
///
/// scope_domain restricts the obligation to resources inside a failure domain;
/// scope_resource restricts it to a single resource. Both absent means
/// site-wide. A protected obligation is never satisfied by capacity that
/// maintenance has excluded, and never satisfied by capacity whose evidence is
/// unknown.
struct ProtectedObligation {
  ObligationId id;
  ObligationKind kind = ObligationKind::UNKNOWN;
  std::optional<FailureDomainId> scope_domain;
  std::optional<ResourceKey> scope_resource;
  std::uint64_t required = 0;
  bool protected_from_maintenance = true;
  Generation generation;
  Digest record_digest;
  Evidence evidence;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ProtectedObligation&, const ProtectedObligation&) = default;
};

/// The verdict for one obligation.
struct ObligationVerdict {
  ObligationId id;
  ObligationKind kind = ObligationKind::UNKNOWN;
  /// OK when satisfied, OBLIGATION_VIOLATED when breached,
  /// OBLIGATION_INDETERMINATE when the evidence does not settle it.
  Status status = Status::UNKNOWN;
  std::uint64_t required = 0;
  std::uint64_t observed = 0;
  bool observed_known = false;
  SourceSet sources;
  Factors factors;

  [[nodiscard]] bool satisfied() const noexcept { return status == Status::OK; }
  [[nodiscard]] std::string to_string() const;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_MAINTENANCE_HPP
