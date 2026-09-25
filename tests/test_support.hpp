// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared test scaffolding.
//
// The reference model below is deliberately written from the specification
// rather than from the implementation. It shares no code with the composer: it
// groups claims by hand, compares them by value, routes channels from the
// documented rules and sums with a plain accumulator. When it and the composer
// disagree, one of them is wrong and the test says which resource they
// disagreed about.

#ifndef SITE_FABRIC_TESTS_TEST_SUPPORT_HPP
#define SITE_FABRIC_TESTS_TEST_SUPPORT_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/digest.hpp"
#include "internal/crypto.hpp"
#include "site_fabric/site_fabric.hpp"
#include "test_framework.hpp"

namespace sftest {

/// A clock the test drives by hand, so composition instants are inputs rather
/// than surprises.
class ManualClock {
 public:
  explicit ManualClock(std::int64_t start) : now_(start) {}

  [[nodiscard]] std::int64_t now() const noexcept { return now_; }
  void advance(std::int64_t delta) noexcept { now_ += delta; }
  void set(std::int64_t value) noexcept { now_ = value; }
  [[nodiscard]] site_fabric::Clock function() {
    return [this]() { return now_; };
  }

 private:
  std::int64_t now_;
};

/// A directory that removes itself.
class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto thread = static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    path_ = std::filesystem::temp_directory_path() /
            ("site_fabric_test_" + label + "_" + std::to_string(stamp) + "_" +
             std::to_string(thread));
    std::error_code code;
    std::filesystem::remove_all(path_, code);
    std::filesystem::create_directories(path_, code);
  }

  ~TemporaryDirectory() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const {
    return path_ / name;
  }

 private:
  std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// The independent reference model
// ---------------------------------------------------------------------------

/// Which aggregate channels a value feeds, from the documented rules.
struct ReferenceMask {
  bool ingress = false;
  bool egress = false;
  bool internal = false;
};

[[nodiscard]] inline ReferenceMask reference_mask(site_fabric::CapacityScope scope,
                                                  bool external) {
  using site_fabric::CapacityScope;
  switch (scope) {
    case CapacityScope::SITE_INGRESS:
      return ReferenceMask{true, false, false};
    case CapacityScope::SITE_EGRESS:
      return ReferenceMask{false, true, false};
    case CapacityScope::SITE_INTERNAL:
    case CapacityScope::RACK_LOCAL:
    case CapacityScope::POD_LOCAL:
    case CapacityScope::CLUSTER_LOCAL:
      return ReferenceMask{false, false, true};
    case CapacityScope::SHARED_LINK:
      return external ? ReferenceMask{true, true, false} : ReferenceMask{false, false, true};
    case CapacityScope::GATEWAY:
      return ReferenceMask{true, true, false};
    default:
      return ReferenceMask{};
  }
}

/// The value of one claim, as the reference model compares it.
struct ReferenceValue {
  site_fabric::CapacityScope scope = site_fabric::CapacityScope::UNKNOWN;
  bool external = false;
  site_fabric::CapacityVector capacity;

  friend bool operator==(const ReferenceValue& left, const ReferenceValue& right) {
    return left.scope == right.scope && left.external == right.external &&
           left.capacity == right.capacity;
  }
};

struct ReferenceTotals {
  std::uint64_t ingress = 0;
  std::uint64_t egress = 0;
  std::uint64_t internal = 0;
  bool ingress_seen = false;
  bool egress_seen = false;
  bool internal_seen = false;
  bool ingress_known = true;
  bool egress_known = true;
  bool internal_known = true;
  std::size_t counted = 0;
  std::vector<std::string> conflicts;
  std::vector<std::string> owners;

  [[nodiscard]] site_fabric::CapacityVector total() const {
    site_fabric::CapacityVector out;
    if (ingress_seen && ingress_known) {
      out.ingress = site_fabric::CapacityValue(ingress);
    }
    if (egress_seen && egress_known) {
      out.egress = site_fabric::CapacityValue(egress);
    }
    if (internal_seen && internal_known) {
      out.internal = site_fabric::CapacityValue(internal);
    }
    return out;
  }
};

/// Recomputes the site capacity totals by brute force.
///
/// Only declarations from members in the authoritative set contribute, and only
/// when every attestor of a resource agrees on the value.
[[nodiscard]] inline site_fabric::Status reference_capacity(
    const std::vector<site_fabric::MemberDomainDeclaration>& declarations,
    const std::set<site_fabric::MemberDomainKey>& authoritative, ReferenceTotals& out) {
  using namespace site_fabric;

  std::map<ResourceKey, std::vector<ReferenceValue>> grouped;

  for (const auto& declaration : declarations) {
    if (authoritative.find(declaration.domain) == authoritative.end()) {
      continue;
    }
    for (const auto& claim : declaration.racks) {
      ReferenceValue value;
      value.scope = CapacityScope::RACK_LOCAL;
      value.capacity = claim.local_capacity;
      grouped[ResourceKey::rack(claim.id.value())].push_back(value);
    }
    for (const auto& claim : declaration.shared_links) {
      ReferenceValue value;
      value.scope = CapacityScope::SHARED_LINK;
      value.external = claim.external;
      value.capacity = claim.capacity;
      grouped[ResourceKey::shared_link(claim.id.value())].push_back(value);
    }
    for (const auto& claim : declaration.gateways) {
      ReferenceValue value;
      value.scope = CapacityScope::GATEWAY;
      value.external = true;
      value.capacity = claim.capacity;
      grouped[ResourceKey::gateway(claim.id.value())].push_back(value);
    }
    for (const auto& claim : declaration.capacity) {
      ReferenceValue value;
      value.scope = claim.scope;
      value.capacity = claim.capacity;
      grouped[claim.owner].push_back(value);
    }
  }

  for (const auto& [key, values] : grouped) {
    if (values.empty()) {
      continue;
    }
    const ReferenceValue& first = values.front();
    bool agreed = true;
    for (const auto& value : values) {
      if (!(value == first)) {
        agreed = false;
        break;
      }
    }
    if (!agreed) {
      out.conflicts.push_back(key.to_string());
      continue;
    }

    const ReferenceMask mask = reference_mask(first.scope, first.external);
    if (!mask.ingress && !mask.egress && !mask.internal) {
      continue;
    }
    ++out.counted;
    out.owners.push_back(key.to_string());

    if (mask.ingress) {
      if (!first.capacity.ingress.known) {
        out.ingress_seen = true;
        out.ingress_known = false;
      } else if (!out.ingress_seen || out.ingress_known) {
        out.ingress_seen = true;
        out.ingress += first.capacity.ingress.bps;
      }
    }
    if (mask.egress) {
      if (!first.capacity.egress.known) {
        out.egress_seen = true;
        out.egress_known = false;
      } else if (!out.egress_seen || out.egress_known) {
        out.egress_seen = true;
        out.egress += first.capacity.egress.bps;
      }
    }
    if (mask.internal) {
      if (!first.capacity.internal.known) {
        out.internal_seen = true;
        out.internal_known = false;
      } else if (!out.internal_seen || out.internal_known) {
        out.internal_seen = true;
        out.internal += first.capacity.internal.bps;
      }
    }
  }

  std::sort(out.conflicts.begin(), out.conflicts.end());
  std::sort(out.owners.begin(), out.owners.end());
  return site_fabric::Status::OK;
}

/// A second, hierarchical reference: the internal capacity of a cluster is the
/// sum of the local capacity of the racks the cluster claims. Nothing in the
/// composer computes it this way.
[[nodiscard]] inline std::uint64_t reference_cluster_internal(
    const site_fabric::MemberDomainDeclaration& cluster_domain,
    const std::map<std::string, std::uint64_t>& rack_internal) {
  std::uint64_t total = 0;
  for (const auto& claim : cluster_domain.clusters) {
    for (const auto& rack : claim.racks) {
      const auto found = rack_internal.find(rack.value());
      if (found != rack_internal.end()) {
        total += found->second;
      }
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// A single sealed declaration with one rack, for focused tests.
[[nodiscard]] inline site_fabric::MemberDomainDeclaration simple_rack_declaration(
    const std::string& domain_id, const std::string& rack_id, std::uint64_t internal_bps,
    std::int64_t observed_at, std::uint64_t ttl,
    site_fabric::Provenance provenance = site_fabric::Provenance::REPORTED) {
  site_fabric::MemberDomainDeclaration declaration;
  declaration.domain = site_fabric::MemberDomainKey::rack(domain_id);
  declaration.site = site_fabric::SiteId::unchecked("site-alpha");
  declaration.generation = site_fabric::Generation::initial();
  declaration.observed_epoch = site_fabric::Epoch::initial();
  site_fabric::Evidence evidence;
  evidence.provenance = provenance;
  evidence.observed_at_ms = observed_at;
  evidence.ttl_ms = ttl;
  declaration.evidence = evidence;

  site_fabric::RackClaim claim;
  claim.id = site_fabric::RackId::unchecked(rack_id);
  claim.generation = site_fabric::Generation::initial();
  claim.local_capacity = site_fabric::CapacityVector(
      site_fabric::CapacityValue::unknown(), site_fabric::CapacityValue::unknown(),
      site_fabric::CapacityValue(internal_bps));
  claim.uplink = site_fabric::ConnectivityState::UP;
  claim.evidence = evidence;
  declaration.racks.push_back(std::move(claim));
  (void)declaration.seal();
  return declaration;
}

/// Adds an external shared link, which is what gives a site an ingress and an
/// egress aggregate. Without one the site cannot be CURRENT, and that is the
/// point: a site that never reported external capacity has not reported it.
inline void add_external_link(site_fabric::MemberDomainDeclaration& declaration,
                              const std::string& id, std::uint64_t rate, std::int64_t observed,
                              std::uint64_t ttl = 60000) {
  site_fabric::SharedLinkClaim claim;
  claim.id = site_fabric::LinkId::unchecked(id);
  claim.external = true;
  claim.generation = site_fabric::Generation::initial();
  claim.capacity = site_fabric::CapacityVector(site_fabric::CapacityValue(rate),
                                               site_fabric::CapacityValue(rate),
                                               site_fabric::CapacityValue::unknown());
  claim.state = site_fabric::ConnectivityState::UP;
  claim.evidence = site_fabric::Evidence::reported(observed, ttl);
  declaration.shared_links.push_back(std::move(claim));
}

inline void add_gateway(site_fabric::MemberDomainDeclaration& declaration,
                        const std::string& id, std::uint64_t rate, std::int64_t observed,
                        std::uint64_t ttl = 60000) {
  site_fabric::GatewayClaim claim;
  claim.id = site_fabric::GatewayId::unchecked(id);
  claim.generation = site_fabric::Generation::initial();
  claim.capacity = site_fabric::CapacityVector(site_fabric::CapacityValue(rate),
                                               site_fabric::CapacityValue(rate),
                                               site_fabric::CapacityValue::unknown());
  claim.state = site_fabric::ConnectivityState::UP;
  claim.evidence = site_fabric::Evidence::reported(observed, ttl);
  declaration.gateways.push_back(std::move(claim));
}

/// A one-rack declaration under an explicit member-domain key, for tests that
/// need two different domains to make a claim about the same resource.
[[nodiscard]] inline site_fabric::MemberDomainDeclaration simple_rack_declaration_for(
    const site_fabric::MemberDomainKey& domain, const std::string& rack_id,
    std::uint64_t internal_bps, std::int64_t observed_at, std::uint64_t ttl,
    site_fabric::Provenance provenance = site_fabric::Provenance::REPORTED) {
  site_fabric::MemberDomainDeclaration declaration = simple_rack_declaration(
      domain.id, rack_id, internal_bps, observed_at, ttl, provenance);
  declaration.domain = domain;
  (void)declaration.seal();
  return declaration;
}

/// Builds an expectation over a set of declarations.
[[nodiscard]] inline site_fabric::SiteExpectation expectation_over(
    const std::vector<site_fabric::MemberDomainDeclaration>& declarations,
    bool required = true) {
  site_fabric::SiteExpectation expectation;
  expectation.site = site_fabric::SiteId::unchecked("site-alpha");
  for (const auto& declaration : declarations) {
    site_fabric::ExpectedMember member;
    member.domain = declaration.domain;
    member.required = required;
    expectation.members.push_back(member);
  }
  (void)expectation.canonicalize();
  return expectation;
}

/// Publishes a set of declarations into one composition input.
[[nodiscard]] inline site_fabric::CompositionInput input_over(
    const std::vector<site_fabric::MemberDomainDeclaration>& declarations,
    std::int64_t now_ms, site_fabric::Generation generation = site_fabric::Generation(1)) {
  site_fabric::CompositionInput input;
  input.site = site_fabric::SiteId::unchecked("site-alpha");
  input.epoch = site_fabric::Epoch::initial();
  input.generation = generation;
  input.incarnation = site_fabric::Incarnation(1, site_fabric::BootNonce(std::string("test")));
  input.now_ms = now_ms;
  input.expectation = expectation_over(declarations);
  std::uint64_t sequence = 1;
  for (const auto& declaration : declarations) {
    site_fabric::MemberPublicationRecord record;
    record.domain = declaration.domain;
    record.generation = declaration.generation;
    record.digest = declaration.digest;
    record.schema = declaration.schema;
    record.site = declaration.site;
    record.observed_epoch = declaration.observed_epoch;
    record.incarnation = input.incarnation;
    record.publisher = site_fabric::MemberId::unchecked("test");
    record.attempt = site_fabric::PublishAttempt{sequence, 1};
    record.received_at_ms = now_ms;
    record.acceptance_sequence = sequence;
    record.has_declaration = true;
    record.declaration = declaration;
    input.publications.push_back(std::move(record));
    ++sequence;
  }
  return input;
}

}  // namespace sftest

#endif  // SITE_FABRIC_TESTS_TEST_SUPPORT_HPP
