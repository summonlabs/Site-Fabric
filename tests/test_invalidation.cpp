// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

SyntheticConfig two_cluster_config() {
  SyntheticConfig config;
  config.clusters = 2;
  config.pods_per_cluster = 1;
  config.racks_per_pod = 2;
  config.shared_links = 2;
  config.gateways = 1;
  config.failure_domains = 2;
  config.maintenance_zones = 0;
  config.obligations = 0;
  return config;
}

}  // namespace

SF_TEST(invalidation, decisions_name_their_exact_sources) {
  SyntheticConfig config = two_cluster_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));

  SF_CHECK(!site.decisions.empty());
  for (const auto& decision : site.decisions) {
    if (decision.id.kind == DecisionKind::CAPACITY_TOTAL ||
        decision.id.kind == DecisionKind::SITE_LIFECYCLE ||
        decision.id.kind == DecisionKind::MEMBERSHIP_COMPLETENESS) {
      SF_CHECK(!decision.sources.empty());
    }
  }
}

SF_TEST(invalidation, a_change_invalidates_only_dependent_decisions) {
  SyntheticConfig config = two_cluster_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));

  // Take the source of one rack member domain in the first cluster.
  SourceRef changed;
  bool found = false;
  for (const auto& member : site.members) {
    if (member.domain.kind == MemberDomainKind::POD &&
        member.domain.id.find("pod-0-") == 0) {
      changed = member.source;
      found = true;
      break;
    }
  }
  SF_REQUIRE(found);

  const InvalidationReport report = invalidate_for_source(site, changed);
  SF_CHECK_EQ(changed.domain, report.domain);
  SF_CHECK(!report.invalidated.empty());
  SF_CHECK(!report.retained.empty());
  SF_CHECK_EQ(site.decisions.size(), report.invalidated.size() + report.retained.size());

  // Every invalidated decision really did depend on the change.
  for (const auto& id : report.invalidated) {
    const SiteDecision* decision = site.find_decision(id);
    SF_REQUIRE(decision != nullptr);
    SF_CHECK(decision->depends_on(changed));
  }
  // And no retained decision did.
  for (const auto& id : report.retained) {
    const SiteDecision* decision = site.find_decision(id);
    SF_REQUIRE(decision != nullptr);
    SF_CHECK(!decision->depends_on(changed));
  }

  // A rack owned by the other cluster is untouched by this change.
  const ResourceAttribution* other = nullptr;
  for (const auto& attribution : site.ownership) {
    if (attribution.key.kind == ResourceKind::RACK &&
        attribution.key.id.find("rack-1-") == 0) {
      other = &attribution;
      break;
    }
  }
  SF_REQUIRE(other != nullptr);
  const SiteDecision* other_decision = nullptr;
  for (const auto& decision : site.decisions) {
    if (decision.id.kind == DecisionKind::OWNERSHIP_MAP && decision.id.subject == other->key) {
      other_decision = &decision;
      break;
    }
  }
  SF_REQUIRE(other_decision != nullptr);
  SF_CHECK(std::find(report.retained.begin(), report.retained.end(), other_decision->id) !=
           report.retained.end());
}

SF_TEST(invalidation, a_new_generation_does_not_invalidate_the_old_one) {
  SyntheticConfig config = two_cluster_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));

  SourceRef original;
  for (const auto& member : site.members) {
    if (member.domain.kind == MemberDomainKind::POD) {
      original = member.source;
      break;
    }
  }
  SF_REQUIRE(original.valid());

  const SourceRef same_domain_new_generation(original.domain, original.generation.next(),
                                             internal::digest_text("moved on"));
  const InvalidationReport report = invalidate_for_source(site, same_domain_new_generation);
  // Nothing in this composition was derived from that exact edge.
  SF_CHECK(report.invalidated.empty());
  SF_CHECK_EQ(site.decisions.size(), report.retained.size());
}

SF_TEST(invalidation, domain_level_invalidation_covers_every_decision_that_mentions_it) {
  SyntheticConfig config = two_cluster_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));

  const MemberDomainKey domain = MemberDomainKey::cluster("cluster-0");
  const InvalidationReport report = invalidate_for_domain(site, domain);
  SF_CHECK(!report.invalidated.empty());
  for (const auto& id : report.invalidated) {
    const SiteDecision* decision = site.find_decision(id);
    SF_REQUIRE(decision != nullptr);
    SF_CHECK(decision->depends_on_domain(domain));
  }
  SF_CHECK(report.invalidates_everything());
}

SF_TEST(invalidation, lower_level_change_leaves_unrelated_capacity_authoritative) {
  SyntheticConfig config = two_cluster_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(config, generated)));
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(generated.input, site)));
  SF_REQUIRE(site.capacity.total.internal.known);
  const std::uint64_t before = site.capacity.total.internal.bps;

  // Change only the racks of the first pod domain and recompose.
  CompositionInput changed_input = generated.input;
  for (auto& publication : changed_input.publications) {
    if (publication.domain.kind != MemberDomainKind::POD ||
        publication.domain.id.find("pod-0-") != 0) {
      continue;
    }
    for (auto& claim : publication.declaration.racks) {
      claim.local_capacity.internal = CapacityValue(claim.local_capacity.internal.bps + 7);
      claim.record_digest = Digest::zero();
    }
    publication.declaration.generation = publication.declaration.generation.next();
    SF_REQUIRE(is_ok(publication.declaration.seal()));
    publication.generation = publication.declaration.generation;
    publication.digest = publication.declaration.digest;
  }
  ComposedSite recomposed;
  SF_REQUIRE(is_ok(composer.compose(changed_input, recomposed)));
  SF_REQUIRE(recomposed.capacity.total.internal.known);
  SF_CHECK_NE(before, recomposed.capacity.total.internal.bps);

  // The other cluster's rack decisions are unaffected in kind; only the
  // decisions that actually consumed the changed source moved.
  const ResourceAttribution* other = nullptr;
  for (const auto& attribution : recomposed.ownership) {
    if (attribution.key.kind == ResourceKind::RACK &&
        attribution.key.id.find("rack-1-") == 0) {
      other = &attribution;
      break;
    }
  }
  SF_REQUIRE(other != nullptr);
  SF_CHECK(other->attributed());
  SF_CHECK_EQ(Status::OK, other->status);
}
