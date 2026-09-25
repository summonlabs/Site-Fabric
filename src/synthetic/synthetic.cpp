// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The deterministic scenario generator.
//
// Every value here is produced from the seed and the configuration alone. There
// is no clock read, no random_device, and no dependency on iteration order of
// any hash container, so a failing seed replays exactly.

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "site_fabric/synthetic.hpp"

namespace site_fabric {
namespace {

[[nodiscard]] std::string number(std::uint64_t value) { return std::to_string(value); }

[[nodiscard]] CapacityVector make_capacity(std::uint64_t ingress, std::uint64_t egress,
                                           std::uint64_t internal) {
  return CapacityVector(CapacityValue(ingress), CapacityValue(egress), CapacityValue(internal));
}

}  // namespace

std::uint64_t SyntheticRandom::next_u64() {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::uint32_t SyntheticRandom::next_u32() {
  return static_cast<std::uint32_t>(next_u64() >> 32U);
}

std::uint64_t SyntheticRandom::bounded(std::uint64_t bound) {
  if (bound == 0) {
    return 0;
  }
  return next_u64() % bound;
}

std::int64_t SyntheticRandom::range(std::int64_t low, std::int64_t high) {
  if (high <= low) {
    return low;
  }
  const std::uint64_t span = static_cast<std::uint64_t>(high - low);
  return low + static_cast<std::int64_t>(bounded(span + 1));
}

bool SyntheticRandom::chance(std::uint64_t numerator, std::uint64_t denominator) {
  if (denominator == 0) {
    return false;
  }
  return bounded(denominator) < numerator;
}

std::string SyntheticRandom::token(std::string_view prefix, std::size_t width) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(prefix);
  for (std::size_t index = 0; index < width; ++index) {
    out.push_back(kDigits[next_u64() & 0x0FU]);
  }
  return out;
}

std::size_t SyntheticSite::distinct_rack_count() const {
  std::vector<RackId> copy = rack_ids;
  std::sort(copy.begin(), copy.end());
  copy.erase(std::unique(copy.begin(), copy.end()), copy.end());
  return copy.size();
}

std::size_t SyntheticSite::distinct_link_count() const {
  std::vector<LinkId> copy = link_ids;
  std::sort(copy.begin(), copy.end());
  copy.erase(std::unique(copy.begin(), copy.end()), copy.end());
  return copy.size();
}

Status generate_synthetic_site(const SyntheticConfig& config, SyntheticSite& out) {
  out = SyntheticSite{};

  if (config.clusters == 0 || config.pods_per_cluster == 0 || config.racks_per_pod == 0) {
    return Status::INVALID;
  }
  const std::size_t total_pods = config.clusters * config.pods_per_cluster;
  if (total_pods > limits::kMaxMemberDomains / 4 ||
      total_pods * config.racks_per_pod > limits::kMaxClaimLists) {
    return Status::LIMIT_EXCEEDED;
  }
  if (config.shared_links > limits::kMaxDistinctResources) {
    return Status::LIMIT_EXCEEDED;
  }
  if (config.epoch.is_set() == false || config.generation.is_set() == false) {
    return Status::INVALID;
  }

  SyntheticRandom random(config.seed);

  // --- Names --------------------------------------------------------------
  for (std::size_t cluster = 0; cluster < config.clusters; ++cluster) {
    out.cluster_ids.push_back(ClusterId::unchecked("cluster-" + number(cluster)));
  }
  for (std::size_t cluster = 0; cluster < config.clusters; ++cluster) {
    for (std::size_t pod = 0; pod < config.pods_per_cluster; ++pod) {
      out.pod_ids.push_back(PodId::unchecked("pod-" + number(cluster) + "-" + number(pod)));
    }
  }
  for (std::size_t cluster = 0; cluster < config.clusters; ++cluster) {
    for (std::size_t pod = 0; pod < config.pods_per_cluster; ++pod) {
      for (std::size_t rack = 0; rack < config.racks_per_pod; ++rack) {
        out.rack_ids.push_back(RackId::unchecked("rack-" + number(cluster) + "-" + number(pod) +
                                                 "-" + number(rack)));
      }
    }
  }
  for (std::size_t index = 0; index < config.shared_links; ++index) {
    out.link_ids.push_back(LinkId::unchecked("link-" + number(index)));
  }
  for (std::size_t index = 0; index < config.gateways; ++index) {
    out.gateway_ids.push_back(GatewayId::unchecked("gw-" + number(index)));
  }
  for (std::size_t index = 0; index < config.failure_domains; ++index) {
    out.failure_domain_ids.push_back(FailureDomainId::unchecked("fd-" + number(index)));
  }

  const auto rack_index = [&](std::size_t cluster, std::size_t pod, std::size_t rack) {
    return (cluster * config.pods_per_cluster + pod) * config.racks_per_pod + rack;
  };
  const auto pod_index = [&](std::size_t cluster, std::size_t pod) {
    return cluster * config.pods_per_cluster + pod;
  };

  // Which pods use which link. A link with two users is the normal case and the
  // reason site capacity cannot be a plain sum.
  std::vector<std::vector<std::size_t>> link_users(config.shared_links);
  for (std::size_t index = 0; index < config.shared_links; ++index) {
    const std::size_t first = index % total_pods;
    link_users[index].push_back(first);
    if (config.share_links_across_clusters && total_pods > 1) {
      const std::size_t second = (index + 1) % total_pods;
      if (second != first) {
        link_users[index].push_back(second);
      }
    }
    std::sort(link_users[index].begin(), link_users[index].end());
  }

  std::vector<std::uint64_t> link_rates;
  link_rates.reserve(config.shared_links);
  for (std::size_t index = 0; index < config.shared_links; ++index) {
    const std::uint64_t jitter = random.bounded(config.base_link_bps / 8 + 1);
    link_rates.push_back(config.base_link_bps + jitter);
  }

  const Provenance provenance = config.provenance;
  const auto evidence_for = [&](std::int64_t observed) {
    Evidence evidence;
    evidence.provenance = provenance;
    evidence.observed_at_ms = observed;
    evidence.ttl_ms = config.ttl_ms;
    // No payload digest is supplied: the fixture asserts a model, it does not
    // attest to bytes, and claiming a digest it never computed would be a lie
    // in exactly the field a consumer would trust.
    return evidence;
  };

  // --- Cluster member domains --------------------------------------------
  for (std::size_t cluster = 0; cluster < config.clusters; ++cluster) {
    MemberDomainDeclaration declaration;
    declaration.domain = MemberDomainKey::cluster(out.cluster_ids[cluster].value());
    declaration.site = config.site;
    declaration.generation = Generation::initial();
    declaration.observed_epoch = config.epoch;

    const std::int64_t observed =
        (config.stale_member && cluster == 0) ? config.now_ms - 10 * static_cast<std::int64_t>(
                                                                  config.ttl_ms)
                                              : config.now_ms;
    declaration.evidence = evidence_for(observed);

    ClusterClaim cluster_claim;
    cluster_claim.id = out.cluster_ids[cluster];
    cluster_claim.generation = Generation::initial();
    for (std::size_t pod = 0; pod < config.pods_per_cluster; ++pod) {
      cluster_claim.pods.push_back(out.pod_ids[pod_index(cluster, pod)]);
      for (std::size_t rack = 0; rack < config.racks_per_pod; ++rack) {
        cluster_claim.racks.push_back(out.rack_ids[rack_index(cluster, pod, rack)]);
      }
    }
    cluster_claim.evidence = evidence_for(observed);
    declaration.clusters.push_back(std::move(cluster_claim));

    for (std::size_t pod = 0; pod < config.pods_per_cluster; ++pod) {
      PodClaim pod_claim;
      pod_claim.id = out.pod_ids[pod_index(cluster, pod)];
      pod_claim.generation = Generation::initial();
      for (std::size_t rack = 0; rack < config.racks_per_pod; ++rack) {
        pod_claim.racks.push_back(out.rack_ids[rack_index(cluster, pod, rack)]);
      }
      pod_claim.evidence = evidence_for(observed);
      declaration.pods.push_back(std::move(pod_claim));
    }

    CapacityContribution pool;
    pool.owner = ResourceKey::capacity_pool("pool-cluster-" + number(cluster));
    pool.scope = CapacityScope::CLUSTER_LOCAL;
    pool.generation = Generation::initial();
    pool.capacity = make_capacity(0, 0, config.base_local_bps * config.racks_per_pod);
    pool.evidence = evidence_for(observed);
    declaration.capacity.push_back(std::move(pool));

    if (cluster < config.failure_domains) {
      FailureDomainClaim domain;
      domain.id = out.failure_domain_ids[cluster];
      domain.domain_class = (cluster % 2 == 0) ? FailureDomainClass::PHYSICAL
                                               : FailureDomainClass::ADMINISTRATIVE;
      domain.members.push_back(ResourceKey::cluster(out.cluster_ids[cluster].value()));
      for (std::size_t pod = 0; pod < config.pods_per_cluster; ++pod) {
        domain.members.push_back(
            ResourceKey::pod(out.pod_ids[pod_index(cluster, pod)].value()));
        for (std::size_t rack = 0; rack < config.racks_per_pod; ++rack) {
          domain.members.push_back(
              ResourceKey::rack(out.rack_ids[rack_index(cluster, pod, rack)].value()));
        }
      }
      if (config.cycle_failure_domains && config.failure_domains >= 2) {
        const std::size_t other = (cluster + 1) % config.failure_domains;
        if (other != cluster) {
          domain.parent = out.failure_domain_ids[other];
        }
      } else if (config.dangling_parent && cluster == 0) {
        domain.parent = FailureDomainId::unchecked("fd-absent");
      } else if (cluster > 0 && cluster < config.failure_domains) {
        domain.parent = out.failure_domain_ids[0];
      }
      domain.generation = Generation::initial();
      domain.evidence = evidence_for(observed);
      declaration.failure_domains.push_back(std::move(domain));
    }

    if (cluster < config.maintenance_zones) {
      MaintenanceZone zone;
      zone.id = MaintenanceZoneId::unchecked("mz-" + number(cluster));
      zone.state = config.active_maintenance
                       ? MaintenanceState::ACTIVE
                       : (config.unknown_maintenance ? MaintenanceState::UNKNOWN
                                                     : MaintenanceState::SCHEDULED);
      zone.window_start_ms = config.now_ms - 1000;
      zone.window_end_ms = config.now_ms + 3600 * 1000;
      zone.domains.push_back(out.failure_domain_ids.empty()
                                 ? FailureDomainId::unchecked("fd-0")
                                 : out.failure_domain_ids[cluster % config.failure_domains]);
      zone.generation = Generation::initial();
      zone.evidence = evidence_for(observed);
      declaration.maintenance_zones.push_back(std::move(zone));
    }

    if (config.site_mismatch && cluster == 0) {
      declaration.site = SiteId::unchecked("site-elsewhere");
    }
    if (config.schema_mismatch && cluster == 0) {
      declaration.schema = SchemaVersion{kModelSchemaMajor + 1, 0};
    }

    const Status sealed = declaration.seal();
    if (!is_ok(sealed)) {
      return sealed;
    }
    if (config.corrupt_digest && cluster == 0) {
      declaration.digest.bytes[0] = static_cast<std::uint8_t>(declaration.digest.bytes[0] ^ 0xFFU);
    }
    out.declarations.push_back(std::move(declaration));
  }

  // --- Pod member domains -------------------------------------------------
  for (std::size_t cluster = 0; cluster < config.clusters; ++cluster) {
    for (std::size_t pod = 0; pod < config.pods_per_cluster; ++pod) {
      const std::size_t index = pod_index(cluster, pod);
      MemberDomainDeclaration declaration;
      declaration.domain = MemberDomainKey::pod(out.pod_ids[index].value());
      declaration.site = config.site;
      declaration.generation = Generation::initial();
      declaration.observed_epoch = config.epoch;
      declaration.evidence = evidence_for(config.now_ms);

      PodClaim pod_claim;
      pod_claim.id = out.pod_ids[index];
      pod_claim.generation = Generation::initial();
      for (std::size_t rack = 0; rack < config.racks_per_pod; ++rack) {
        pod_claim.racks.push_back(out.rack_ids[rack_index(cluster, pod, rack)]);
      }
      pod_claim.evidence = evidence_for(config.now_ms);
      declaration.pods.push_back(std::move(pod_claim));

      for (std::size_t rack = 0; rack < config.racks_per_pod; ++rack) {
        RackClaim rack_claim;
        rack_claim.id = out.rack_ids[rack_index(cluster, pod, rack)];
        rack_claim.generation = Generation::initial();
        rack_claim.local_capacity =
            make_capacity(0, 0, config.base_local_bps + random.bounded(config.base_local_bps / 10));
        rack_claim.uplink = ConnectivityState::UP;
        rack_claim.evidence = evidence_for(config.now_ms);
        declaration.racks.push_back(std::move(rack_claim));
      }

      // A deliberate overlap: the first rack of a second pod is claimed again,
      // with different capacity, by a different member domain.
      if (config.duplicate_ownership && cluster == 0 && pod == 1 && config.pods_per_cluster > 1) {
        RackClaim overlap;
        overlap.id = out.rack_ids[rack_index(0, 0, 0)];
        overlap.generation = Generation::initial();
        overlap.local_capacity = make_capacity(0, 0, config.base_local_bps * 3);
        overlap.uplink = ConnectivityState::DEGRADED;
        overlap.evidence = evidence_for(config.now_ms);
        declaration.racks.push_back(std::move(overlap));
      }

      for (std::size_t link = 0; link < config.shared_links; ++link) {
        const auto& users = link_users[link];
        if (std::find(users.begin(), users.end(), index) == users.end()) {
          continue;
        }
        SharedLinkClaim claim;
        claim.id = out.link_ids[link];
        claim.external = (link % 2 == 0);
        claim.generation = Generation::initial();
        std::uint64_t rate = link_rates[link];
        if (config.conflicting_shared_link && link == 0 && users.size() > 1 &&
            users.front() != index) {
          rate = rate / 2;
        }
        if (claim.external) {
          claim.capacity = make_capacity(rate, rate, 0);
        } else {
          claim.capacity = make_capacity(0, 0, rate);
        }
        if (config.unknown_capacity && link == 0) {
          // Every user of the link leaves the channel unreported, so there is
          // no disagreement to report: the value is simply not known.
          claim.capacity.egress = CapacityValue::unknown();
        }
        claim.state = ConnectivityState::UP;
        claim.evidence = evidence_for(config.now_ms);
        declaration.shared_links.push_back(std::move(claim));
      }

      const Status sealed = declaration.seal();
      if (!is_ok(sealed)) {
        return sealed;
      }
      out.declarations.push_back(std::move(declaration));
    }
  }

  // --- Shared-resource member domain -------------------------------------
  {
    MemberDomainDeclaration declaration;
    declaration.domain = MemberDomainKey::shared_resource("fabric-shared");
    declaration.site = config.site;
    declaration.generation = Generation::initial();
    declaration.observed_epoch = config.epoch;
    declaration.evidence = evidence_for(config.now_ms);

    for (std::size_t index = 0; index < config.gateways; ++index) {
      GatewayClaim claim;
      claim.id = out.gateway_ids[index];
      claim.generation = Generation::initial();
      const std::uint64_t rate = config.base_link_bps * 2 + random.bounded(config.base_link_bps);
      claim.capacity = make_capacity(rate, rate, 0);
      claim.state = ConnectivityState::UP;
      claim.evidence = evidence_for(config.now_ms);
      declaration.gateways.push_back(std::move(claim));
    }

    CapacityContribution ingress;
    ingress.owner = ResourceKey::capacity_pool("pool-site-ingress");
    ingress.scope = CapacityScope::SITE_INGRESS;
    ingress.generation = Generation::initial();
    ingress.capacity = make_capacity(config.base_link_bps * 4, 0, 0);
    ingress.evidence = evidence_for(config.now_ms);
    declaration.capacity.push_back(std::move(ingress));

    CapacityContribution egress;
    egress.owner = ResourceKey::capacity_pool("pool-site-egress");
    egress.scope = CapacityScope::SITE_EGRESS;
    egress.generation = Generation::initial();
    egress.capacity = make_capacity(0, config.base_link_bps * 4, 0);
    egress.evidence = evidence_for(config.now_ms);
    declaration.capacity.push_back(std::move(egress));

    for (std::size_t index = 0; index < config.obligations; ++index) {
      ProtectedObligation obligation;
      obligation.id = ObligationId::unchecked("ob-" + number(index));
      obligation.generation = Generation::initial();
      obligation.required = 1;
      switch (index % 3) {
        case 0:
          obligation.kind = ObligationKind::MIN_UP_SHARED_LINKS;
          obligation.required = config.impossible_obligation
                                    ? static_cast<std::uint64_t>(config.shared_links) + 100
                                    : 1;
          break;
        case 1: {
          obligation.kind = ObligationKind::MIN_INTERNAL_CAPACITY;
          // The bound is the largest value a capacity obligation may carry, so
          // an impossible floor is expressed as that bound rather than as a
          // number the declaration validator would refuse outright.
          obligation.required = config.impossible_obligation
                                    ? limits::kMaxCapacityBps
                                    : config.base_local_bps;
          break;
        }
        default:
          obligation.kind = ObligationKind::MIN_UP_GATEWAYS;
          obligation.required = config.gateways == 0 ? 0 : 1;
          break;
      }
      obligation.evidence = evidence_for(config.now_ms);
      declaration.obligations.push_back(std::move(obligation));
    }

    const Status sealed = declaration.seal();
    if (!is_ok(sealed)) {
      return sealed;
    }
    out.declarations.push_back(std::move(declaration));
  }

  out.expectation = expectation_from_declarations(config.site, out.declarations, false,
                                                  config.missing_member || config.generation_mismatch);
  if (config.missing_member) {
    ExpectedMember absent;
    absent.domain = MemberDomainKey::rack("rack-absent");
    absent.required = true;
    out.expectation.members.push_back(absent);
  }
  if (config.generation_mismatch && !out.expectation.members.empty()) {
    out.expectation.members.front().pinned = true;
    out.expectation.members.front().generation = Generation(7);
    out.expectation.members.front().digest = Digest::zero();
  }
  const Status canonical = out.expectation.canonicalize();
  if (!is_ok(canonical)) {
    return canonical;
  }

  out.input = composition_input_from(config.site, config.epoch, config.generation, config.now_ms,
                                     out.declarations);
  return Status::OK;
}

SiteExpectation expectation_from_declarations(const SiteId& site,
                                              const std::vector<MemberDomainDeclaration>&
                                                  declarations,
                                              bool pinned, bool required) {
  SiteExpectation expectation;
  expectation.site = site;
  expectation.minimum_epoch = Epoch::initial();
  for (const auto& declaration : declarations) {
    ExpectedMember member;
    member.domain = declaration.domain;
    member.pinned = pinned;
    member.generation = declaration.generation;
    member.digest = declaration.digest;
    member.required = required;
    expectation.members.push_back(std::move(member));
  }
  return expectation;
}

CompositionInput composition_input_from(const SiteId& site, Epoch epoch, Generation generation,
                                        std::int64_t now_ms,
                                        const std::vector<MemberDomainDeclaration>& declarations) {
  CompositionInput input;
  input.site = site;
  input.epoch = epoch;
  input.generation = generation;
  input.incarnation = Incarnation(1, BootNonce(std::string("fixture")));
  input.now_ms = now_ms;
  input.expectation = expectation_from_declarations(site, declarations, false, true);

  std::uint64_t sequence = 1;
  for (const auto& declaration : declarations) {
    MemberPublicationRecord record;
    record.domain = declaration.domain;
    record.generation = declaration.generation;
    record.digest = declaration.digest;
    record.schema = declaration.schema;
    record.site = declaration.site;
    record.observed_epoch = declaration.observed_epoch;
    record.incarnation = input.incarnation;
    record.publisher = MemberId::unchecked("fixture-" + declaration.domain.id);
    record.attempt = PublishAttempt{sequence, 1};
    record.received_at_ms = now_ms;
    record.acceptance_sequence = sequence;
    record.has_declaration = true;
    record.declaration = declaration;
    input.publications.push_back(std::move(record));
    ++sequence;
  }
  return input;
}

}  // namespace site_fabric
