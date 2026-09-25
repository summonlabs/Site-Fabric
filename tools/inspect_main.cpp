// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// site_fabric_inspect - read-only inspection.
//
// Three modes: compose a generated scenario and print the result, read a store
// and summarise what survived, or query a running controller over loopback and
// print the snapshot it currently holds. It never publishes anything.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "site_fabric/persistence.hpp"
#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"

namespace {

void usage() {
  std::fprintf(stderr, "usage: site_fabric_inspect scenario [--seed N] [--clusters N]\n");
  std::fprintf(stderr, "                                 [--pods N] [--racks N] [--links N]\n");
  std::fprintf(stderr, "                                 [--gateways N] [--domains N] [--json]\n");
  std::fprintf(stderr, "       site_fabric_inspect store PATH\n");
  std::fprintf(stderr, "       site_fabric_inspect controller [--host H] --port N [--site S]\n");
  std::fprintf(stderr, "       site_fabric_inspect versions\n");
}

void print_site(const site_fabric::ComposedSite& site, bool json) {
  if (json) {
    std::printf("{\n");
    std::printf("  \"site\": \"%s\",\n", site.site.value().c_str());
    std::printf("  \"epoch\": %llu,\n",
                static_cast<unsigned long long>(site.epoch.value()));
    std::printf("  \"generation\": %llu,\n",
                static_cast<unsigned long long>(site.generation.value()));
    std::printf("  \"lifecycle\": \"%s\",\n", site_fabric::to_string(site.lifecycle));
    std::printf("  \"status\": \"%s\",\n", site_fabric::to_string(site.status));
    std::printf("  \"membership_complete\": %s,\n",
                site.membership_complete ? "true" : "false");
    std::printf("  \"members\": %zu,\n", site.members.size());
    std::printf("  \"conflicts\": %zu,\n", site.conflicts.size());
    std::printf("  \"deficits\": %zu,\n", site.deficits.size());
    std::printf("  \"capacity_closes\": %s,\n",
                site.capacity.closes_exactly ? "true" : "false");
    std::printf("  \"digest\": \"%s\"\n", site.compute_digest().to_string().c_str());
    std::printf("}\n");
    return;
  }

  std::printf("%s\n", site_fabric::explain(site).c_str());
  std::printf("\nmembers:\n");
  for (const auto& member : site.members) {
    std::printf("  %s\n", member.to_string().c_str());
  }
  std::printf("\nownership:\n");
  for (const auto& attribution : site.ownership) {
    std::printf("  %s\n", attribution.to_string().c_str());
  }
  std::printf("\ncapacity ledger:\n");
  for (const auto& entry : site.capacity.entries) {
    std::printf("  %s\n", entry.to_string().c_str());
  }
  std::printf("\nfailure domains:\n");
  for (const auto& node : site.failure_domains.nodes) {
    std::printf("  %s\n", node.to_string().c_str());
  }
  std::printf("\nmaintenance:\n");
  for (const auto& effect : site.maintenance) {
    std::printf("  %s\n", effect.to_string().c_str());
  }
  std::printf("\nobligations:\n");
  for (const auto& verdict : site.obligations) {
    std::printf("  %s\n", verdict.to_string().c_str());
  }
  std::printf("\ndecisions: %zu\n", site.decisions.size());
  std::printf("site digest: %s\n", site.compute_digest().to_string().c_str());
}

int inspect_scenario(int argc, char** argv, int start) {
  site_fabric::SyntheticConfig config;
  bool json = false;
  for (int index = start; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        return nullptr;
      }
      return argv[++index];
    };
    if (argument == "--seed") {
      const char* text = value("--seed");
      if (text == nullptr) {
        return 2;
      }
      config.seed = std::strtoull(text, nullptr, 10);
    } else if (argument == "--clusters") {
      const char* text = value("--clusters");
      if (text == nullptr) {
        return 2;
      }
      config.clusters = static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--pods") {
      const char* text = value("--pods");
      if (text == nullptr) {
        return 2;
      }
      config.pods_per_cluster = static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--racks") {
      const char* text = value("--racks");
      if (text == nullptr) {
        return 2;
      }
      config.racks_per_pod = static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--links") {
      const char* text = value("--links");
      if (text == nullptr) {
        return 2;
      }
      config.shared_links = static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--gateways") {
      const char* text = value("--gateways");
      if (text == nullptr) {
        return 2;
      }
      config.gateways = static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--domains") {
      const char* text = value("--domains");
      if (text == nullptr) {
        return 2;
      }
      config.failure_domains = static_cast<std::size_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--json") {
      json = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
      return 2;
    }
  }

  site_fabric::SyntheticSite generated;
  const site_fabric::Status status = site_fabric::generate_synthetic_site(config, generated);
  if (!site_fabric::is_ok(status)) {
    std::fprintf(stderr, "scenario refused: %s\n", site_fabric::to_string(status));
    return 3;
  }
  site_fabric::SiteComposer composer;
  site_fabric::ComposedSite site;
  const site_fabric::Status composed = composer.compose(generated.input, site);
  if (!site_fabric::is_ok(composed)) {
    std::fprintf(stderr, "compose refused: %s\n", site_fabric::to_string(composed));
    return 3;
  }
  if (!json) {
    std::printf("scenario seed=%llu (SYNTHETIC fixture data, provenance %s)\n",
                static_cast<unsigned long long>(config.seed),
                site_fabric::to_string(config.provenance));
  }
  print_site(site, json);
  return 0;
}

int inspect_store(const std::string& path) {
  site_fabric::StoreContents contents;
  const site_fabric::Status status = site_fabric::SiteStore::read_file(path, contents);
  std::printf("recovery: %s\n", contents.recovery.to_string().c_str());
  for (const auto& diagnostic : contents.recovery.diagnostics) {
    std::printf("  diagnostic: %s\n", diagnostic.c_str());
  }
  std::printf("snapshots=%zu publications=%zu retirements=%zu tombstones=%zu leases=%zu\n",
              contents.snapshot_history.size(), contents.publications.size(),
              contents.retirements.size(), contents.tombstones.size(), contents.leases.size());
  if (contents.has_snapshot) {
    std::printf("latest: %s\n", contents.latest_snapshot.to_string().c_str());
    print_site(contents.latest_snapshot.state, false);
  }
  if (!site_fabric::is_ok(status) && status != site_fabric::Status::TRUNCATED) {
    std::fprintf(stderr, "store status: %s\n", site_fabric::to_string(status));
    return 3;
  }
  return 0;
}

int inspect_controller(int argc, char** argv, int start) {
  site_fabric::PublisherConfig config;
  config.port = 0;
  config.site = site_fabric::SiteId::unchecked("site-alpha");
  for (int index = start; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        return nullptr;
      }
      return argv[++index];
    };
    if (argument == "--host") {
      const char* text = value("--host");
      if (text == nullptr) {
        return 2;
      }
      config.host = text;
    } else if (argument == "--port") {
      const char* text = value("--port");
      if (text == nullptr) {
        return 2;
      }
      config.port = static_cast<std::uint16_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--site") {
      const char* text = value("--site");
      if (text == nullptr) {
        return 2;
      }
      config.site = site_fabric::SiteId::unchecked(text);
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
      return 2;
    }
  }
  if (config.port == 0) {
    usage();
    return 2;
  }

  config.domain = site_fabric::MemberDomainKey::shared_resource("inspector");
  config.publisher = site_fabric::MemberId::unchecked("inspector");
  config.incarnation = site_fabric::Incarnation(0, site_fabric::BootNonce(std::string("inspect")));

  std::unique_ptr<site_fabric::MemberPublisher> publisher;
  const site_fabric::Status created = site_fabric::MemberPublisher::create(config, publisher);
  if (!site_fabric::is_ok(created)) {
    std::fprintf(stderr, "publisher refused: %s\n", site_fabric::to_string(created));
    return 3;
  }
  const site_fabric::Status connected = publisher->connect();
  if (!site_fabric::is_ok(connected)) {
    std::fprintf(stderr, "connect failed: %s\n", site_fabric::to_string(connected));
    return 4;
  }
  site_fabric::FetchSiteResponse response;
  const site_fabric::Status fetched = publisher->fetch_site(0, response);
  if (!site_fabric::is_ok(fetched) || !response.has_snapshot) {
    std::fprintf(stderr, "snapshot unavailable: %s\n", site_fabric::to_string(fetched));
    (void)publisher->close();
    return 5;
  }
  std::printf("snapshot: %s\n", response.snapshot.to_string().c_str());
  print_site(response.snapshot.state, false);
  (void)publisher->close();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "scenario") {
    return inspect_scenario(argc, argv, 2);
  }
  if (mode == "store") {
    if (argc < 3) {
      usage();
      return 2;
    }
    return inspect_store(argv[2]);
  }
  if (mode == "controller") {
    return inspect_controller(argc, argv, 2);
  }
  if (mode == "versions") {
    std::printf("%s\n", site_fabric::build_description().c_str());
    std::printf("%s\n", site_fabric::format_versions().c_str());
    return 0;
  }
  usage();
  return 2;
}
