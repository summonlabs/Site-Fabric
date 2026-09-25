// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

/// A rack domain that also declares an external link, so the site has a source
/// for its ingress and egress aggregates and can be judged on its own terms.
MemberDomainDeclaration rack_domain(const std::string& id, const std::string& rack,
                                    std::uint64_t internal_bps, std::int64_t observed) {
  MemberDomainDeclaration declaration =
      sftest::simple_rack_declaration(id, rack, internal_bps, observed, 60000);
  sftest::add_external_link(declaration, "link-" + id, internal_bps, observed);
  (void)declaration.seal();
  return declaration;
}

MemberDomainDeclaration maintenance_domain(const std::string& id, const std::string& zone,
                                           MaintenanceState state,
                                           std::vector<ResourceKey> resources,
                                           std::int64_t observed) {
  MemberDomainDeclaration declaration;
  declaration.domain = MemberDomainKey::shared_resource(id);
  declaration.site = SiteId::unchecked("site-alpha");
  declaration.generation = Generation::initial();
  declaration.observed_epoch = Epoch::initial();
  declaration.evidence = Evidence::reported(observed, 60000);
  MaintenanceZone zone_claim;
  zone_claim.id = MaintenanceZoneId::unchecked(zone);
  zone_claim.state = state;
  zone_claim.window_start_ms = observed - 1000;
  zone_claim.window_end_ms = observed + 60000;
  zone_claim.resources = std::move(resources);
  zone_claim.generation = Generation::initial();
  zone_claim.evidence = declaration.evidence;
  declaration.maintenance_zones.push_back(std::move(zone_claim));
  return declaration;
}

}  // namespace

SF_TEST(maintenance, active_zone_excludes_named_resources) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(rack_domain("rack-a", "r-a", 1000, 1000000));
  declarations.push_back(rack_domain("rack-b", "r-b", 2000, 1000000));
  declarations.push_back(maintenance_domain("ops", "mz-1", MaintenanceState::ACTIVE,
                                            {ResourceKey::rack("r-a")}, 1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(std::uint64_t(3000), site.capacity.total.internal.bps);
  SF_CHECK_EQ(std::uint64_t(2000), site.capacity.available.internal.bps);
  SF_CHECK_EQ(std::uint64_t(1000), site.capacity.excluded_by_maintenance.internal.bps);
  SF_CHECK(site.capacity.closes_exactly);
  SF_REQUIRE(!site.maintenance.empty());
  SF_CHECK_EQ(Status::OK, site.maintenance.front().status);
  SF_CHECK_EQ(std::size_t(1), site.maintenance.front().excluded.size());
}

SF_TEST(maintenance, scheduled_zone_excludes_nothing) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(rack_domain("rack-a", "r-a", 1000, 1000000));
  declarations.push_back(maintenance_domain("ops", "mz-1", MaintenanceState::SCHEDULED,
                                            {ResourceKey::rack("r-a")}, 1000000));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_CHECK_EQ(std::uint64_t(1000), site.capacity.available.internal.bps);
  SF_CHECK_EQ(std::uint64_t(0), site.capacity.excluded_by_maintenance.internal.bps);
}

SF_TEST(maintenance, completed_and_cancelled_zones_exclude_nothing) {
  for (const MaintenanceState state : {MaintenanceState::COMPLETE, MaintenanceState::CANCELLED}) {
    std::vector<MemberDomainDeclaration> declarations;
    declarations.push_back(rack_domain("rack-a", "r-a", 1000, 1000000));
    declarations.push_back(
        maintenance_domain("ops", "mz-1", state, {ResourceKey::rack("r-a")}, 1000000));
    for (auto& declaration : declarations) {
      SF_REQUIRE(is_ok(declaration.seal()));
    }
    SiteComposer composer;
    ComposedSite site;
    SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
    SF_CHECK_EQ(std::uint64_t(1000), site.capacity.available.internal.bps);
  }
}

SF_TEST(maintenance, zone_over_protected_obligation_is_refused) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(rack_domain("rack-a", "r-a", 100000, 1000000));
  declarations.push_back(maintenance_domain("ops", "mz-1", MaintenanceState::ACTIVE,
                                            {ResourceKey::rack("r-a")}, 1000000));

  MemberDomainDeclaration obligation_domain;
  obligation_domain.domain = MemberDomainKey::shared_resource("slo");
  obligation_domain.site = SiteId::unchecked("site-alpha");
  obligation_domain.generation = Generation::initial();
  obligation_domain.observed_epoch = Epoch::initial();
  obligation_domain.evidence = Evidence::reported(1000000, 60000);
  ProtectedObligation obligation;
  obligation.id = ObligationId::unchecked("keep-a");
  obligation.kind = ObligationKind::MIN_INTERNAL_CAPACITY;
  obligation.scope_resource = ResourceKey::rack("r-a");
  obligation.required = 50000;
  obligation.protected_from_maintenance = true;
  obligation.generation = Generation::initial();
  obligation.evidence = obligation_domain.evidence;
  obligation_domain.obligations.push_back(std::move(obligation));
  declarations.push_back(std::move(obligation_domain));

  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));

  // The zone is refused, so nothing is excluded and the site says why.
  SF_CHECK_EQ(std::uint64_t(0), site.capacity.excluded_by_maintenance.internal.bps);
  SF_CHECK_EQ(std::uint64_t(100000), site.capacity.available.internal.bps);
  SF_REQUIRE(!site.maintenance.empty());
  SF_CHECK_EQ(Status::MAINTENANCE_CONFLICT, site.maintenance.front().status);
  SF_CHECK_EQ(std::size_t(1), site.maintenance.front().overlapping_obligations.size());
  SF_CHECK_EQ(SiteLifecycle::CONFLICTING, site.lifecycle);
  bool found = false;
  for (const auto& conflict : site.conflicts) {
    if (conflict.code == Status::MAINTENANCE_CONFLICT) {
      found = true;
    }
  }
  SF_CHECK(found);
}

SF_TEST(maintenance, site_wide_obligation_does_not_block_maintenance) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(rack_domain("rack-a", "r-a", 100000, 1000000));
  declarations.push_back(rack_domain("rack-b", "r-b", 100000, 1000000));
  declarations.push_back(maintenance_domain("ops", "mz-1", MaintenanceState::ACTIVE,
                                            {ResourceKey::rack("r-a")}, 1000000));

  MemberDomainDeclaration obligation_domain;
  obligation_domain.domain = MemberDomainKey::shared_resource("slo");
  obligation_domain.site = SiteId::unchecked("site-alpha");
  obligation_domain.generation = Generation::initial();
  obligation_domain.observed_epoch = Epoch::initial();
  obligation_domain.evidence = Evidence::reported(1000000, 60000);
  ProtectedObligation obligation;
  obligation.id = ObligationId::unchecked("keep-site");
  obligation.kind = ObligationKind::MIN_INTERNAL_CAPACITY;
  obligation.required = 150000;
  obligation.generation = Generation::initial();
  obligation.evidence = obligation_domain.evidence;
  obligation_domain.obligations.push_back(std::move(obligation));
  declarations.push_back(std::move(obligation_domain));
  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  SF_REQUIRE(!site.maintenance.empty());
  SF_CHECK_EQ(Status::OK, site.maintenance.front().status);
  SF_CHECK_EQ(std::uint64_t(100000), site.capacity.excluded_by_maintenance.internal.bps);
  // The site-wide floor is evaluated against available capacity, and it now
  // fails: maintenance is allowed, the promise is not silently kept.
  SF_REQUIRE(!site.obligations.empty());
  SF_CHECK_EQ(Status::OBLIGATION_VIOLATED, site.obligations.front().status);
  SF_CHECK_EQ(SiteLifecycle::DEGRADED, site.lifecycle);
}

SF_TEST(maintenance, zone_scope_follows_nested_domains) {
  std::vector<MemberDomainDeclaration> declarations;
  declarations.push_back(rack_domain("rack-a", "r-a", 1000, 1000000));
  declarations.push_back(rack_domain("rack-b", "r-b", 2000, 1000000));

  MemberDomainDeclaration parent_domain;
  parent_domain.domain = MemberDomainKey::shared_resource("fabric");
  parent_domain.site = SiteId::unchecked("site-alpha");
  parent_domain.generation = Generation::initial();
  parent_domain.observed_epoch = Epoch::initial();
  parent_domain.evidence = Evidence::reported(1000000, 60000);
  FailureDomainClaim parent;
  parent.id = FailureDomainId::unchecked("fd-row");
  parent.domain_class = FailureDomainClass::PHYSICAL;
  parent.members = {ResourceKey::rack("r-a")};
  parent.generation = Generation::initial();
  parent.evidence = parent_domain.evidence;
  parent_domain.failure_domains.push_back(std::move(parent));
  declarations.push_back(std::move(parent_domain));

  MemberDomainDeclaration child_domain;
  child_domain.domain = MemberDomainKey::shared_resource("row-ops");
  child_domain.site = SiteId::unchecked("site-alpha");
  child_domain.generation = Generation::initial();
  child_domain.observed_epoch = Epoch::initial();
  child_domain.evidence = Evidence::reported(1000000, 60000);
  FailureDomainClaim child;
  child.id = FailureDomainId::unchecked("fd-rack-b");
  child.domain_class = FailureDomainClass::PHYSICAL;
  child.parent = FailureDomainId::unchecked("fd-row");
  child.members = {ResourceKey::rack("r-b")};
  child.generation = Generation::initial();
  child.evidence = child_domain.evidence;
  child_domain.failure_domains.push_back(std::move(child));

  MaintenanceZone zone;
  zone.id = MaintenanceZoneId::unchecked("mz-row");
  zone.state = MaintenanceState::ACTIVE;
  zone.domains = {FailureDomainId::unchecked("fd-row")};
  zone.generation = Generation::initial();
  zone.evidence = child_domain.evidence;
  child_domain.maintenance_zones.push_back(std::move(zone));
  declarations.push_back(std::move(child_domain));

  for (auto& declaration : declarations) {
    SF_REQUIRE(is_ok(declaration.seal()));
  }

  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(sftest::input_over(declarations, 1000000), site)));
  // Both the row's own rack and the nested child's rack are in scope.
  SF_CHECK_EQ(std::uint64_t(3000), site.capacity.total.internal.bps);
  SF_CHECK_EQ(std::uint64_t(0), site.capacity.available.internal.bps);
  SF_REQUIRE(!site.maintenance.empty());
  SF_CHECK_EQ(std::size_t(2), site.maintenance.front().excluded.size());
}

SF_TEST(maintenance, zone_with_empty_scope_is_invalid) {
  MemberDomainDeclaration declaration;
  declaration.domain = MemberDomainKey::shared_resource("ops");
  declaration.site = SiteId::unchecked("site-alpha");
  declaration.generation = Generation::initial();
  declaration.observed_epoch = Epoch::initial();
  declaration.evidence = Evidence::reported(1000000, 60000);
  MaintenanceZone zone;
  zone.id = MaintenanceZoneId::unchecked("mz-empty");
  zone.state = MaintenanceState::ACTIVE;
  zone.generation = Generation::initial();
  zone.evidence = declaration.evidence;
  declaration.maintenance_zones.push_back(std::move(zone));
  SF_CHECK_EQ(Status::INVALID, declaration.seal());
}
