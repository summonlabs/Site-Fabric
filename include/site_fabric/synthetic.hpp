// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic scenario generation.
//
// Everything here is SYNTHETIC. The generator exists so that property tests,
// differential tests and benchmarks can build large hierarchical sites without
// a data centre, and so that a failing case can be replayed from its seed
// alone. A site assembled from these declarations can never be reported as a
// healthy physical site: the declarations carry Provenance::SYNTHETIC and the
// composer treats that as a knowledge gap, not as evidence.

#ifndef SITE_FABRIC_SYNTHETIC_HPP
#define SITE_FABRIC_SYNTHETIC_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "site_fabric/composition.hpp"

namespace site_fabric {

/// splitmix64. Small, fast, fully specified, and identical on every platform,
/// which is what makes a failing seed reproducible.
class SyntheticRandom {
 public:
  explicit SyntheticRandom(std::uint64_t seed_value) : state_(seed_value) {}

  [[nodiscard]] std::uint64_t next_u64();
  [[nodiscard]] std::uint32_t next_u32();
  /// Uniform in [0, bound). Returns 0 when bound is 0.
  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound);
  [[nodiscard]] std::int64_t range(std::int64_t low, std::int64_t high);
  /// True with probability numerator/denominator.
  [[nodiscard]] bool chance(std::uint64_t numerator, std::uint64_t denominator);
  [[nodiscard]] std::string token(std::string_view prefix, std::size_t width = 6);

  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

struct SyntheticConfig {
  std::uint64_t seed = 0x5F1E5FAB1234ULL;
  SiteId site{"site-alpha"};

  std::size_t clusters = 3;
  std::size_t pods_per_cluster = 2;
  std::size_t racks_per_pod = 3;
  std::size_t shared_links = 4;
  std::size_t gateways = 2;
  std::size_t failure_domains = 3;
  std::size_t maintenance_zones = 1;
  std::size_t obligations = 2;

  std::uint64_t base_link_bps = 400ULL * 1000ULL * 1000ULL * 1000ULL;
  std::uint64_t base_local_bps = 100ULL * 1000ULL * 1000ULL * 1000ULL;

  /// The provenance stamped on every generated declaration.
  ///
  /// The default is REPORTED, which is what a member domain does: it reports
  /// its own subtree. Nothing here measures anything, and the generator never
  /// claims otherwise. Setting this to SYNTHETIC exercises the rule that
  /// synthetic evidence can never make a site CURRENT.
  Provenance provenance = Provenance::REPORTED;
  std::int64_t now_ms = 1'700'000'000'000LL;
  std::uint64_t ttl_ms = 60'000;

  Epoch epoch = Epoch::initial();
  Generation generation = Generation::initial();

  // --- Fault injection. Each flag is independent. --------------------------

  /// Shared links are declared by every cluster that touches them, which is
  /// the normal case and the reason aggregation cannot be a sum.
  bool share_links_across_clusters = true;
  /// Two clusters claim the same rack.
  bool duplicate_ownership = false;
  /// Two clusters claim the same shared link with different capacity.
  bool conflicting_shared_link = false;
  /// Skip one required member domain entirely.
  bool missing_member = false;
  /// Report a member domain, but with expired evidence.
  bool stale_member = false;
  /// Leave one capacity channel unreported.
  bool unknown_capacity = false;
  /// Make the failure-domain graph cyclic.
  bool cycle_failure_domains = false;
  /// Point a failure domain at a parent that does not exist.
  bool dangling_parent = false;
  /// Put a maintenance zone in ACTIVE state over a live rack.
  bool active_maintenance = false;
  /// Leave a maintenance zone in UNKNOWN state.
  bool unknown_maintenance = false;
  /// Declare an obligation that the generated capacity cannot meet.
  bool impossible_obligation = false;
  /// Seal a declaration with a digest that does not match its content.
  bool corrupt_digest = false;
  /// Report the wrong generation for a member domain.
  bool generation_mismatch = false;
  /// Report the wrong site.
  bool site_mismatch = false;
  /// Report a schema the controller does not support.
  bool schema_mismatch = false;
};

/// A generated site: expectation, declarations and a ready-made input.
struct SyntheticSite {
  SiteExpectation expectation;
  std::vector<MemberDomainDeclaration> declarations;
  CompositionInput input;
  std::vector<RackId> rack_ids;
  std::vector<ClusterId> cluster_ids;
  std::vector<PodId> pod_ids;
  std::vector<LinkId> link_ids;
  std::vector<GatewayId> gateway_ids;
  std::vector<FailureDomainId> failure_domain_ids;

  [[nodiscard]] std::size_t distinct_rack_count() const;
  [[nodiscard]] std::size_t distinct_link_count() const;
};

/// Generates a site. Deterministic for a given seed and configuration.
Status generate_synthetic_site(const SyntheticConfig& config, SyntheticSite& out);

/// Builds an expectation that pins every declaration to its exact generation
/// and digest. Used to test the pinned-generation path.
[[nodiscard]] SiteExpectation expectation_from_declarations(
    const SiteId& site, const std::vector<MemberDomainDeclaration>& declarations,
    bool pinned, bool required);

/// Builds an input from a set of declarations.
[[nodiscard]] CompositionInput composition_input_from(const SiteId& site, Epoch epoch,
                                                      Generation generation,
                                                      std::int64_t now_ms,
                                                      const std::vector<MemberDomainDeclaration>&
                                                          declarations);

}  // namespace site_fabric

#endif  // SITE_FABRIC_SYNTHETIC_HPP
