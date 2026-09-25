// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Seeded property tests.
//
// Every case is generated from the seed the runner printed, so a failure
// reports the exact input that produced it. The differential half compares the
// composer against the independent reference model in test_support.hpp, which
// shares no code with it.

#include <algorithm>
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

struct Case {
  SyntheticConfig config;
  std::uint64_t seed = 0;
};

Case make_case(SyntheticRandom& random) {
  Case out;
  out.seed = random.next_u64();
  out.config.seed = out.seed;
  out.config.clusters = 1 + random.bounded(3);
  out.config.pods_per_cluster = 1 + random.bounded(3);
  out.config.racks_per_pod = 1 + random.bounded(3);
  out.config.shared_links = random.bounded(6);
  out.config.gateways = random.bounded(3);
  out.config.failure_domains = random.bounded(4);
  out.config.maintenance_zones = random.bounded(3);
  out.config.obligations = random.bounded(3);
  out.config.base_link_bps = 1000 + random.bounded(1000000);
  out.config.base_local_bps = 1000 + random.bounded(1000000);
  out.config.now_ms = 1000000 + static_cast<std::int64_t>(random.bounded(100000));
  out.config.ttl_ms = 1000 + random.bounded(120000);
  out.config.provenance = random.chance(3, 4) ? Provenance::REPORTED : Provenance::MEASURED;
  out.config.duplicate_ownership = random.chance(1, 3);
  out.config.conflicting_shared_link = random.chance(1, 4);
  out.config.missing_member = random.chance(1, 5);
  out.config.stale_member = random.chance(1, 4);
  out.config.unknown_capacity = random.chance(1, 5);
  out.config.cycle_failure_domains = random.chance(1, 6);
  out.config.dangling_parent = random.chance(1, 6);
  out.config.active_maintenance = random.chance(1, 3);
  out.config.unknown_maintenance = random.chance(1, 8);
  out.config.impossible_obligation = random.chance(1, 5);
  return out;
}

void report(const std::string& message, std::uint64_t seed, int iteration) {
  sftest::record_failure(__FILE__, __LINE__,
                         message + " seed=" + std::to_string(seed) +
                             " iteration=" + std::to_string(iteration));
}

}  // namespace

SF_TEST(property, composition_invariants_hold_over_random_sites) {
  SyntheticRandom random(sftest::current_seed());
  for (int iteration = 0; iteration < 40; ++iteration) {
    const Case test_case = make_case(random);
    SyntheticSite generated;
    const Status generated_status = generate_synthetic_site(test_case.config, generated);
    if (!is_ok(generated_status)) {
      continue;
    }

    SiteComposer composer;
    ComposedSite site;
    const Status status = composer.compose(generated.input, site);
    if (!is_ok(status)) {
      report("compose refused a generated site: " + std::string(to_string(status)), test_case.seed,
             iteration);
      continue;
    }

    // Determinism: the same input composes to the same site digest.
    ComposedSite again;
    if (!is_ok(composer.compose(generated.input, again)) ||
        !(again.compute_digest() == site.compute_digest())) {
      report("recomposition is not deterministic", test_case.seed, iteration);
    }

    // No member domain appears twice.
    std::set<MemberDomainKey> domains;
    for (const auto& member : site.members) {
      if (!domains.insert(member.domain).second) {
        report("duplicate member state", test_case.seed, iteration);
      }
    }

    // Decisions are unique by id.
    std::vector<std::string> decision_ids;
    for (const auto& decision : site.decisions) {
      decision_ids.push_back(decision.id.to_string());
    }
    std::sort(decision_ids.begin(), decision_ids.end());
    if (std::adjacent_find(decision_ids.begin(), decision_ids.end()) != decision_ids.end()) {
      report("duplicate decision id", test_case.seed, iteration);
    }

    // Conflicting is exactly the presence of a conflict.
    const bool conflicting = site.lifecycle == SiteLifecycle::CONFLICTING;
    if (conflicting != !site.conflicts.empty()) {
      report("lifecycle and conflicts disagree", test_case.seed, iteration);
    }

    // A current site has nothing wrong with it.
    if (site.lifecycle == SiteLifecycle::CURRENT) {
      if (!site.membership_complete || !site.all_evidence_fresh || site.any_conflict ||
          !site.deficits.empty()) {
        report("current site is not clean", test_case.seed, iteration);
      }
      for (const auto& member : site.members) {
        if (member.lifecycle != MemberLifecycle::CURRENT) {
          report("current site has a non-current member", test_case.seed, iteration);
        }
      }
    }

    // Ownership entries are unique by key and carry at least one attestor.
    std::vector<std::string> keys;
    for (const auto& attribution : site.ownership) {
      keys.push_back(attribution.key.to_string());
      if (attribution.attestors.empty()) {
        report("ownership without attestors", test_case.seed, iteration);
      }
    }
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
      report("duplicate ownership entry", test_case.seed, iteration);
    }

    // A counted entry is an agreed one. Nothing conflicting or unsourced may be
    // folded into the aggregate.
    for (const auto& entry : site.capacity.entries) {
      if (!entry.counted_in_total) {
        continue;
      }
      if (entry.status != Status::OK && entry.status != Status::CAPACITY_UNKNOWN) {
        report("a non-OK entry was counted: " + std::string(to_string(entry.status)),
               test_case.seed, iteration);
      }
      if (entry.attestors.empty()) {
        report("a counted entry has no attestor", test_case.seed, iteration);
      }
    }

    // Closing: when the ledger says it closes, available plus excluded really
    // does reproduce the total channel by channel.
    if (site.capacity.closes_exactly) {
      if (site.capacity.status == Status::CAPACITY_OVERFLOW) {
        report("ledger claims to close despite overflow", test_case.seed, iteration);
      }
      if (!capacity_leq(site.capacity.available, site.capacity.total) ||
          !capacity_leq(site.capacity.excluded_by_maintenance, site.capacity.total)) {
        report("closing identity violates monotonicity", test_case.seed, iteration);
      }
      CapacityVector recombined = site.capacity.available;
      if (is_ok(add_capacity(recombined, site.capacity.excluded_by_maintenance))) {
        if (site.capacity.total.ingress.known && recombined.ingress.known &&
            recombined.ingress.bps != site.capacity.total.ingress.bps) {
          report("ingress does not recombine", test_case.seed, iteration);
        }
        if (site.capacity.total.egress.known && recombined.egress.known &&
            recombined.egress.bps != site.capacity.total.egress.bps) {
          report("egress does not recombine", test_case.seed, iteration);
        }
        if (site.capacity.total.internal.known && recombined.internal.known &&
            recombined.internal.bps != site.capacity.total.internal.bps) {
          report("internal does not recombine", test_case.seed, iteration);
        }
      }
    }

    // Differential: every resource the reference model counts is either counted
    // or explained, and the totals agree over the authoritative set.
    std::set<MemberDomainKey> authoritative;
    for (const auto& member : site.members) {
      if (member.lifecycle == MemberLifecycle::CURRENT) {
        authoritative.insert(member.domain);
      }
    }
    sftest::ReferenceTotals reference;
    if (!is_ok(sftest::reference_capacity(generated.declarations, authoritative, reference))) {
      report("reference model refused the input", test_case.seed, iteration);
      continue;
    }
    if (site.capacity.status != Status::CAPACITY_OVERFLOW) {
      const CapacityVector expected = reference.total();
      if (expected.ingress.known != site.capacity.total.ingress.known ||
          expected.egress.known != site.capacity.total.egress.known ||
          expected.internal.known != site.capacity.total.internal.known) {
        report("channel knowledge disagrees with the reference model", test_case.seed, iteration);
      }
      if (expected.ingress.known && expected.ingress.bps != site.capacity.total.ingress.bps) {
        report("ingress total disagrees with the reference model", test_case.seed, iteration);
      }
      if (expected.egress.known && expected.egress.bps != site.capacity.total.egress.bps) {
        report("egress total disagrees with the reference model", test_case.seed, iteration);
      }
      if (expected.internal.known && expected.internal.bps != site.capacity.total.internal.bps) {
        report("internal total disagrees with the reference model", test_case.seed, iteration);
      }
    }
  }
}

SF_TEST(property, maintenance_never_increases_available_capacity) {
  SyntheticRandom random(sftest::current_seed());
  for (int iteration = 0; iteration < 25; ++iteration) {
    Case test_case = make_case(random);
    test_case.config.active_maintenance = true;
    test_case.config.maintenance_zones = 1 + random.bounded(2);
    SyntheticSite generated;
    if (!is_ok(generate_synthetic_site(test_case.config, generated))) {
      continue;
    }
    SiteComposer composer;
    ComposedSite site;
    if (!is_ok(composer.compose(generated.input, site))) {
      continue;
    }
    if (!site.capacity.closes_exactly) {
      report("ledger does not close", test_case.seed, iteration);
    }
    if (!capacity_leq(site.capacity.available, site.capacity.total)) {
      report("available exceeds total", test_case.seed, iteration);
    }
    if (!capacity_leq(site.capacity.excluded_by_maintenance, site.capacity.total)) {
      report("excluded exceeds total", test_case.seed, iteration);
    }
  }
}

SF_TEST(property, a_pinned_expectation_is_never_satisfied_by_a_different_digest) {
  SyntheticRandom random(sftest::current_seed());
  for (int iteration = 0; iteration < 20; ++iteration) {
    const Case test_case = make_case(random);
    SyntheticSite generated;
    if (!is_ok(generate_synthetic_site(test_case.config, generated))) {
      continue;
    }
    CompositionInput input = generated.input;
    for (auto& member : input.expectation.members) {
      member.pinned = true;
      member.generation = Generation(1);
      member.digest = internal::digest_text("expected-" + member.domain.to_string());
    }
    if (!is_ok(input.expectation.canonicalize())) {
      continue;
    }
    SiteComposer composer;
    ComposedSite site;
    if (!is_ok(composer.compose(input, site))) {
      continue;
    }
    for (const auto& member : site.members) {
      if (member.lifecycle == MemberLifecycle::CURRENT) {
        report("a pinned expectation accepted a foreign digest", test_case.seed, iteration);
      }
    }
    if (site.lifecycle == SiteLifecycle::CURRENT) {
      report("a pinned expectation produced a current site", test_case.seed, iteration);
    }
  }
}
