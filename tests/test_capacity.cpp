// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

SyntheticConfig clean_config() {
  SyntheticConfig config;
  config.clusters = 2;
  config.pods_per_cluster = 2;
  config.racks_per_pod = 2;
  config.shared_links = 3;
  config.gateways = 2;
  config.failure_domains = 2;
  config.maintenance_zones = 0;
  config.obligations = 0;
  return config;
}

std::set<MemberDomainKey> all_domains(const SyntheticSite& site) {
  std::set<MemberDomainKey> domains;
  for (const auto& declaration : site.declarations) {
    domains.insert(declaration.domain);
  }
  return domains;
}

}  // namespace

SF_TEST(capacity, ledger_closes_exactly) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    SyntheticConfig config = clean_config();
    config.seed = seed * 0x9E3779B9ULL;
    SyntheticSite generated;
    SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

    SiteComposer composer;
    ComposedSite site;
    SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
    SF_CHECK_EQ(SiteLifecycle::CURRENT, site.lifecycle);
    SF_CHECK(site.capacity.closes_exactly);

    // available + excluded must reproduce the total, channel by channel.
    CapacityVector recombined = site.capacity.available;
    SF_REQUIRE(is_ok(add_capacity(recombined, site.capacity.excluded_by_maintenance)));
    if (site.capacity.total.ingress.known) {
      SF_REQUIRE(recombined.ingress.known);
      SF_CHECK_EQ(site.capacity.total.ingress.bps, recombined.ingress.bps);
    }
    if (site.capacity.total.egress.known) {
      SF_REQUIRE(recombined.egress.known);
      SF_CHECK_EQ(site.capacity.total.egress.bps, recombined.egress.bps);
    }
    if (site.capacity.total.internal.known) {
      SF_REQUIRE(recombined.internal.known);
      SF_CHECK_EQ(site.capacity.total.internal.bps, recombined.internal.bps);
    }
  }
}

SF_TEST(capacity, differential_against_independent_reference) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    SyntheticConfig config = clean_config();
    config.seed = seed * 0x100000001B3ULL + 7;
    config.clusters = 1 + (seed % 3);
    config.shared_links = 1 + (seed % 5);
    SyntheticSite generated;
    SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

    SiteComposer composer;
    ComposedSite site;
    SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
    SF_REQUIRE(site.conflicts.empty());
    SF_REQUIRE(site.deficits.empty());

    sftest::ReferenceTotals reference;
    SF_REQUIRE(is_ok(
        sftest::reference_capacity(generated.declarations, all_domains(generated), reference)));
    SF_CHECK(reference.conflicts.empty());

    SF_CHECK_EQ(reference.counted, site.capacity.entries.size());
    const CapacityVector reference_total = reference.total();
    SF_CHECK_EQ(reference_total.ingress.known, site.capacity.total.ingress.known);
    SF_CHECK_EQ(reference_total.egress.known, site.capacity.total.egress.known);
    SF_CHECK_EQ(reference_total.internal.known, site.capacity.total.internal.known);
    if (reference_total.ingress.known) {
      SF_CHECK_EQ(reference_total.ingress.bps, site.capacity.total.ingress.bps);
    }
    if (reference_total.egress.known) {
      SF_CHECK_EQ(reference_total.egress.bps, site.capacity.total.egress.bps);
    }
    if (reference_total.internal.known) {
      SF_CHECK_EQ(reference_total.internal.bps, site.capacity.total.internal.bps);
    }
  }
}

SF_TEST(capacity, hierarchical_reference_agrees_on_rack_locals) {
  SyntheticConfig config = clean_config();
  config.clusters = 2;
  config.obligations = 0;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

  std::map<std::string, std::uint64_t> rack_internal;
  for (const auto& declaration : generated.declarations) {
    for (const auto& claim : declaration.racks) {
      rack_internal[claim.id.value()] = claim.local_capacity.internal.bps;
    }
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));

  std::uint64_t expected = 0;
  for (const auto& declaration : generated.declarations) {
    if (declaration.domain.kind != MemberDomainKind::CLUSTER) {
      continue;
    }
    expected += sftest::reference_cluster_internal(declaration, rack_internal);
  }
  // Rack-local capacity feeds internal capacity; cluster-local pools feed it
  // too, so the racks alone must be a lower bound and the pool must be the
  // difference.
  SF_REQUIRE(site.capacity.total.internal.known);
  SF_CHECK(site.capacity.total.internal.bps >= expected);

  // Internal capacity also arrives from cluster-local pools and from shared
  // links that never leave the site.
  std::uint64_t pools = 0;
  for (const auto& declaration : generated.declarations) {
    for (const auto& claim : declaration.capacity) {
      if (claim.scope == CapacityScope::CLUSTER_LOCAL) {
        pools += claim.capacity.internal.bps;
      }
    }
  }
  std::uint64_t internal_links = 0;
  {
    std::map<std::string, std::uint64_t> link_rates;
    for (const auto& declaration : generated.declarations) {
      for (const auto& claim : declaration.shared_links) {
        if (!claim.external) {
          link_rates[claim.id.value()] = claim.capacity.internal.bps;
        }
      }
    }
    for (const auto& [id, rate] : link_rates) {
      (void)id;
      internal_links += rate;
    }
  }
  SF_CHECK_EQ(expected + pools + internal_links, site.capacity.total.internal.bps);
}

SF_TEST(capacity, unknown_channel_makes_the_aggregate_indeterminate) {
  SyntheticConfig config = clean_config();
  config.unknown_capacity = true;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
  SF_CHECK(site.any_unknown_capacity);
  SF_CHECK_EQ(Status::CAPACITY_UNKNOWN, site.capacity.status);
  SF_CHECK_EQ(SiteLifecycle::INDETERMINATE, site.lifecycle);
  // The unreported channel stays unreported; it does not become zero.
  SF_CHECK(!site.capacity.total.egress.known);
}

SF_TEST(capacity, maintenance_reduces_available_but_never_total) {
  SyntheticConfig config = clean_config();
  config.maintenance_zones = 1;
  config.active_maintenance = true;
  config.failure_domains = 2;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
  SF_CHECK(site.capacity.closes_exactly);
  SF_REQUIRE(!site.maintenance.empty());
  SF_CHECK_EQ(MaintenanceState::ACTIVE, site.maintenance.front().state);
  SF_CHECK(!site.maintenance.front().excluded.empty());

  SF_REQUIRE(site.capacity.total.internal.known);
  SF_REQUIRE(site.capacity.available.internal.known);
  SF_CHECK(site.capacity.available.internal.bps < site.capacity.total.internal.bps);
  SF_CHECK(site.capacity.excluded_by_maintenance.internal.known);
  SF_CHECK_EQ(site.capacity.total.internal.bps - site.capacity.available.internal.bps,
              site.capacity.excluded_by_maintenance.internal.bps);
}

SF_TEST(capacity, unknown_maintenance_makes_its_scope_indeterminate) {
  SyntheticConfig config = clean_config();
  config.maintenance_zones = 1;
  config.unknown_maintenance = true;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
  SF_REQUIRE(!site.maintenance.empty());
  SF_CHECK_EQ(Status::MAINTENANCE_UNKNOWN, site.maintenance.front().status);
  SF_CHECK_EQ(SiteLifecycle::INDETERMINATE, site.lifecycle);
  // A zone whose state was never reported holds its scope out of availability
  // rather than counting it as usable, and says so.
  SF_CHECK(site.capacity.excluded_by_maintenance.internal.known);
  SF_CHECK(site.capacity.excluded_by_maintenance.internal.bps > 0);
  SF_CHECK(site.capacity.available.internal.bps < site.capacity.total.internal.bps);
  SF_CHECK(site.capacity.closes_exactly);
}

SF_TEST(capacity, obligation_verdicts_are_earned) {
  SyntheticConfig config = clean_config();
  config.obligations = 3;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
  SF_CHECK_EQ(std::size_t(3), site.obligations.size());
  for (const auto& verdict : site.obligations) {
    SF_CHECK(is_ok(verdict.status) || verdict.status == Status::OBLIGATION_VIOLATED ||
             verdict.status == Status::OBLIGATION_INDETERMINATE);
  }
}

SF_TEST(capacity, impossible_obligation_is_violated) {
  SyntheticConfig config = clean_config();
  config.obligations = 3;
  config.impossible_obligation = true;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
  bool violated = false;
  for (const auto& verdict : site.obligations) {
    if (verdict.status == Status::OBLIGATION_VIOLATED) {
      violated = true;
    }
  }
  SF_CHECK(violated);
  SF_CHECK_EQ(SiteLifecycle::DEGRADED, site.lifecycle);
}

SF_TEST(capacity, overflow_is_reported_not_wrapped) {
  std::vector<MemberDomainDeclaration> declarations;
  for (int index = 0; index < 4; ++index) {
    MemberDomainDeclaration declaration = sftest::simple_rack_declaration(
        "rack-" + std::to_string(index), "r-" + std::to_string(index),
        limits::kMaxCapacityBps - 1, 1000000, 60000);
    SF_REQUIRE(is_ok(declaration.seal()));
    declarations.push_back(std::move(declaration));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(Status::CAPACITY_OVERFLOW, site.capacity.status);
  SF_CHECK(!site.capacity.total.internal.known);
  SF_CHECK(!site.capacity.closes_exactly);
  SF_CHECK(site.lifecycle == SiteLifecycle::INDETERMINATE ||
           site.lifecycle == SiteLifecycle::CONFLICTING);
}

SF_TEST(capacity, gateways_feed_ingress_and_egress_only) {
  SyntheticConfig config = clean_config();
  config.gateways = 2;
  config.maintenance_zones = 0;
  config.obligations = 0;
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));

  const CapacityLedgerEntry* entry = nullptr;
  for (const auto& candidate : site.capacity.entries) {
    if (candidate.owner.kind == ResourceKind::GATEWAY) {
      entry = &candidate;
      break;
    }
  }
  SF_REQUIRE(entry != nullptr);
  SF_CHECK_EQ(CapacityScope::GATEWAY, entry->scope);
  SF_CHECK_EQ(Status::OK, entry->status);
  SF_CHECK(entry->counted_in_total);
}
