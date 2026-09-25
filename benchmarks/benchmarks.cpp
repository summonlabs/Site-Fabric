// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Completed-work benchmarks.
//
// Each benchmark does the whole job and reports the work it actually completed,
// not a synthetic inner loop. The sites are SYNTHETIC fixture data; the timings
// say something about the runtime, not about a data centre.

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "site_fabric/persistence.hpp"
#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"

namespace {

class Timer {
 public:
  Timer() : started_(std::chrono::steady_clock::now()) {}

  [[nodiscard]] double ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_)
        .count();
  }

 private:
  std::chrono::steady_clock::time_point started_;
};

void report(const char* name, double milliseconds, const std::string& work) {
  std::printf("%-44s %10.3f ms   %s\n", name, milliseconds, work.c_str());
}

struct SiteShape {
  const char* name;
  std::size_t clusters;
  std::size_t pods;
  std::size_t racks;
  std::size_t links;
  std::size_t gateways;
  std::size_t domains;
};

}  // namespace

int main() {
  std::printf("%s\n", site_fabric::build_description().c_str());
  std::printf("%-44s %10s   %s\n", "benchmark", "time", "completed work");
  std::printf("-------------------------------------------------------------------------------\n");

  const std::vector<SiteShape> shapes = {
      {"small (1 cluster, 2 pods, 2 racks)", 1, 2, 2, 2, 1, 1},
      {"medium (4 clusters, 2 pods, 4 racks)", 4, 2, 4, 8, 2, 4},
      {"large (8 clusters, 4 pods, 4 racks)", 8, 4, 4, 16, 4, 8},
  };

  for (const auto& shape : shapes) {
    site_fabric::SyntheticConfig config;
    config.seed = 0x5F1E5FABULL;
    config.clusters = shape.clusters;
    config.pods_per_cluster = shape.pods;
    config.racks_per_pod = shape.racks;
    config.shared_links = shape.links;
    config.gateways = shape.gateways;
    config.failure_domains = shape.domains;
    config.maintenance_zones = 1;
    config.obligations = 2;

    site_fabric::SyntheticSite generated;
    Timer generation;
    if (!site_fabric::is_ok(site_fabric::generate_synthetic_site(config, generated))) {
      std::printf("scenario %s refused\n", shape.name);
      return 1;
    }
    const double generation_ms = generation.ms();

    site_fabric::SiteComposer composer;
    site_fabric::ComposedSite site;
    Timer composition;
    if (!site_fabric::is_ok(composer.compose(generated.input, site))) {
      std::printf("composition of %s refused\n", shape.name);
      return 1;
    }
    const double composition_ms = composition.ms();

    std::string work = std::to_string(generated.declarations.size()) + " declarations, " +
                       std::to_string(site.members.size()) + " members, " +
                       std::to_string(site.ownership.size()) + " owned resources, " +
                       std::to_string(site.capacity.entries.size()) + " ledger entries, " +
                       std::to_string(site.decisions.size()) + " decisions";
    std::string name = std::string("generate ") + shape.name;
    report(name.c_str(), generation_ms, work);
    name = std::string("compose  ") + shape.name;
    report(name.c_str(), composition_ms, work);

    // Canonical encoding plus digest of the whole composed site.
    Timer digest;
    const site_fabric::Digest composed_digest = site.compute_digest();
    report((std::string("digest   ") + shape.name).c_str(), digest.ms(),
           composed_digest.short_hex() + " over " + std::to_string(site.ownership.size()) +
               " resources");

    // Recomposition under shuffled arrival order.
    site_fabric::CompositionInput shuffled = generated.input;
    for (std::size_t index = 0; index + 1 < shuffled.publications.size(); index += 2) {
      std::swap(shuffled.publications[index], shuffled.publications[index + 1]);
    }
    Timer recomposition;
    site_fabric::ComposedSite again;
    if (!site_fabric::is_ok(composer.compose(shuffled, again))) {
      std::printf("recomposition of %s refused\n", shape.name);
      return 1;
    }
    report((std::string("recompose ") + shape.name).c_str(), recomposition.ms(),
           again.compute_digest() == site.compute_digest() ? "identical site digest"
                                                           : "DIGEST MISMATCH");

    // The whole store path: append every publication and the snapshot, then
    // read it back and verify it.
    site_fabric::StoreConfig store_config;
    store_config.max_bytes = 512ULL * 1024ULL * 1024ULL;
    store_config.max_records = 1000000;
    store_config.durable_commit = false;
    const std::string path =
        std::string("site_fabric_benchmark_") + std::to_string(shape.clusters) + ".sfstore";
    store_config.path = path;

    site_fabric::SiteSnapshot snapshot;
    snapshot.site = site.site;
    snapshot.epoch = site.epoch;
    snapshot.generation = site.generation;
    snapshot.incarnation = site.incarnation;
    snapshot.sequence = 1;
    snapshot.published_at_ms = site.composed_at_ms;
    snapshot.lifecycle = site.lifecycle;
    snapshot.status = site.status;
    snapshot.complete = site.membership_complete;
    snapshot.state = site;
    if (!site_fabric::is_ok(snapshot.recompute_digest())) {
      std::printf("snapshot digest refused\n");
      return 1;
    }

    Timer store_write;
    std::size_t appended = 0;
    {
      site_fabric::SiteStore store;
      if (!site_fabric::is_ok(store.open(store_config))) {
        std::printf("store open refused for %s\n", shape.name);
        return 1;
      }
      for (const auto& publication : generated.input.publications) {
        if (site_fabric::is_ok(store.append_publication(publication))) {
          ++appended;
        }
      }
      (void)store.append_snapshot(snapshot);
      (void)store.close();
    }
    report((std::string("store    ") + shape.name).c_str(), store_write.ms(),
           std::to_string(appended) + " records plus 1 snapshot");

    Timer store_read;
    site_fabric::StoreContents contents;
    const site_fabric::Status read = site_fabric::SiteStore::read_file(path, contents);
    report((std::string("recover  ") + shape.name).c_str(), store_read.ms(),
           std::string(site_fabric::to_string(read)) + ", " +
               std::to_string(contents.publications.size()) + " publications, digest " +
               contents.content_digest().short_hex());

    std::remove(path.c_str());
    std::printf("\n");
  }

  // Authority fencing throughput: how fast can a stale controller be refused?
  {
    site_fabric::AuthorityRegistry registry;
    site_fabric::AuthorityToken token;
    (void)registry.acquire_site_authority("controller-1", site_fabric::SiteId::unchecked("site-a"),
                                          site_fabric::Epoch(2),
                                          site_fabric::Incarnation(
                                              1, site_fabric::BootNonce(std::string("b"))),
                                          site_fabric::Generation(1), 1000000, 60000, token);
    constexpr int kIterations = 200000;
    Timer fencing;
    std::uint64_t refused = 0;
    for (int index = 0; index < kIterations; ++index) {
      const site_fabric::Status status = registry.validate(token, 1000000 + index);
      if (site_fabric::is_ok(status)) {
        ++refused;
      }
    }
    report("authority validate (200k calls)", fencing.ms(),
           std::to_string(refused) + " accepted of " + std::to_string(kIterations));
  }

  // Frame encoding over the wire format.
  {
    site_fabric::PingMessage ping;
    std::vector<std::uint8_t> payload;
    (void)site_fabric::encode_message(ping, payload);
    constexpr int kIterations = 200000;
    Timer frames;
    std::size_t bytes = 0;
    for (int index = 0; index < kIterations; ++index) {
      std::vector<std::uint8_t> frame;
      const site_fabric::Status status =
          site_fabric::encode_frame(site_fabric::MessageType::PING, payload, frame);
      if (site_fabric::is_ok(status)) {
        bytes += frame.size();
      }
    }
    report("encode_frame (200k frames)", frames.ms(),
           std::to_string(bytes) + " bytes framed");
  }

  return 0;
}
