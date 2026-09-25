// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A first look at Site Fabric: build a site from member-domain declarations,
// compose it, read the verdict, and ask what a change would invalidate.
//
// The declarations here come from the synthetic fixture generator, so the
// numbers are SYNTHETIC. Nothing in this example measures a real site.

#include <cstdio>
#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"

int main() {
  std::printf("%s\n\n", site_fabric::build_description().c_str());

  site_fabric::SyntheticConfig config;
  config.seed = 20260101ULL;
  config.clusters = 2;
  config.pods_per_cluster = 2;
  config.racks_per_pod = 3;
  config.shared_links = 4;
  config.gateways = 2;
  config.failure_domains = 2;
  config.maintenance_zones = 1;
  config.active_maintenance = true;
  config.obligations = 2;

  site_fabric::SyntheticSite generated;
  const site_fabric::Status generated_status =
      site_fabric::generate_synthetic_site(config, generated);
  if (!site_fabric::is_ok(generated_status)) {
    std::printf("scenario refused: %s\n", site_fabric::to_string(generated_status));
    return 1;
  }
  std::printf("generated %zu member-domain declarations (SYNTHETIC fixture data)\n",
              generated.declarations.size());
  std::printf("  distinct racks: %zu, distinct shared links: %zu\n",
              generated.distinct_rack_count(), generated.distinct_link_count());

  site_fabric::SiteComposer composer;
  site_fabric::ComposedSite site;
  const site_fabric::Status status = composer.compose(generated.input, site);
  if (!site_fabric::is_ok(status)) {
    std::printf("compose refused: %s\n", site_fabric::to_string(status));
    return 1;
  }

  std::printf("\n%s\n", site_fabric::explain(site).c_str());
  std::printf("\nsite identity: %s\n", site.compute_digest().to_string().c_str());

  // Every authoritative field resolves to the exact member generations it came
  // from. Here is one of them.
  for (const auto& attribution : site.ownership) {
    if (attribution.key.kind != site_fabric::ResourceKind::SHARED_LINK) {
      continue;
    }
    std::printf("\nshared resource %s is attested by:\n", attribution.key.to_string().c_str());
    for (const auto& ref : attribution.attestors.items()) {
      std::printf("  %s\n", ref.to_string().c_str());
    }
    break;
  }

  // Recomposition is deterministic: the same inputs give the same site.
  site_fabric::ComposedSite again;
  if (site_fabric::is_ok(composer.compose(generated.input, again))) {
    std::printf("\nrecomposition digest matches: %s\n",
                again.compute_digest() == site.compute_digest() ? "yes" : "no");
  }

  // What would a change to one member invalidate?
  for (const auto& member : site.members) {
    if (member.domain.kind != site_fabric::MemberDomainKind::POD) {
      continue;
    }
    const site_fabric::InvalidationReport report =
        site_fabric::invalidate_for_source(site, member.source);
    std::printf("\na change to %s invalidates %zu of %zu decisions\n",
                member.domain.to_string().c_str(), report.invalidated.size(),
                site.decisions.size());
    std::printf("  %s\n", report.to_string().c_str());
    break;
  }

  return 0;
}
