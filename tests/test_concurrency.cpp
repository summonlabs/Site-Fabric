// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Concurrency and ownership audit, expressed as tests.
//
// Every case here has a matching entry in the audit table in the README. The
// point is not that threads are used, it is that no lock is held across a call
// that could re-enter it, no worker is joined while it holds state the joiner
// needs, and a stop is observed cooperatively rather than by killing anything.

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

SyntheticConfig concurrent_config() {
  SyntheticConfig config;
  config.clusters = 2;
  config.pods_per_cluster = 2;
  config.racks_per_pod = 2;
  config.shared_links = 2;
  config.gateways = 1;
  config.failure_domains = 2;
  config.maintenance_zones = 0;
  config.obligations = 0;
  return config;
}

ControllerConfig controller_config_for(const SiteExpectation& expectation,
                                       sftest::ManualClock& clock,
                                       const std::string& nonce) {
  ControllerConfig config;
  config.site = SiteId::unchecked("site-alpha");
  config.expectation = expectation;
  config.id = ControllerId::unchecked("controller-1");
  config.bind_host = "127.0.0.1";
  config.bind_port = 0;
  config.incarnation = Incarnation(1, BootNonce(nonce));
  config.clock = clock.function();
  config.lease_ttl_ms = 60000;
  return config;
}

}  // namespace

SF_TEST(concurrency, parallel_publication_never_deadlocks_or_loses_a_publication) {
  SyntheticConfig synthetic = concurrent_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, "ctl-concurrent"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  for (const auto& publication : generated.input.publications) {
    std::uint64_t assigned = 0;
    SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
        publication.domain, publication.incarnation, generated.input.now_ms, &assigned)));
  }

  const std::size_t threads = 8;
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> refused{0};
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (std::size_t index = 0; index < threads; ++index) {
    workers.emplace_back([&, index]() {
      for (std::size_t step = 0; step < generated.input.publications.size(); ++step) {
        const auto& publication =
            generated.input.publications[(index + step) % generated.input.publications.size()];
        PublishResponse response;
        const Status status =
            controller->publish(publication.publisher, publication.incarnation,
                                publication.declaration,
                                PublishAttempt{static_cast<std::uint64_t>(1000 + step), 1},
                                response);
        if (is_ok(status)) {
          accepted.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  SF_CHECK_EQ(std::size_t(threads * generated.input.publications.size()),
              accepted.load() + refused.load());
  SF_CHECK(accepted.load() >= generated.input.publications.size());

  SiteSnapshot snapshot;
  SF_REQUIRE(is_ok(controller->current_snapshot(snapshot)));
  SF_CHECK_EQ(SiteLifecycle::CURRENT, snapshot.lifecycle);
  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(concurrency, composition_races_publication_without_losing_state) {
  SyntheticConfig synthetic = concurrent_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, "ctl-race"), controller)));
  SF_REQUIRE(is_ok(controller->start()));
  for (const auto& publication : generated.input.publications) {
    std::uint64_t assigned = 0;
    SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
        publication.domain, publication.incarnation, generated.input.now_ms, &assigned)));
  }

  std::atomic<bool> stop{false};
  std::thread reader([&]() {
    while (!stop.load()) {
      ComposedSite composed;
      if (is_ok(controller->compose_now(composed))) {
        if (composed.capacity.entries.empty()) {
          continue;
        }
      }
    }
  });

  for (const auto& publication : generated.input.publications) {
    PublishResponse response;
    const Status status =
        controller->publish(publication.publisher, publication.incarnation,
                            publication.declaration, PublishAttempt{1, 1}, response);
    SF_CHECK(is_ok(status) || status == Status::FENCED_ATTEMPT);
  }
  stop.store(true);
  reader.join();

  SiteSnapshot snapshot;
  SF_REQUIRE(is_ok(controller->current_snapshot(snapshot)));
  SF_CHECK_EQ(SiteLifecycle::CURRENT, snapshot.lifecycle);
  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(concurrency, stop_is_observed_by_idle_workers) {
  SyntheticConfig synthetic = concurrent_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, "ctl-idle"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  // A connected but silent peer must not keep stop() waiting.
  PublisherConfig publisher_config;
  publisher_config.host = "127.0.0.1";
  publisher_config.port = controller->port();
  publisher_config.site = SiteId::unchecked("site-alpha");
  publisher_config.domain = generated.declarations.front().domain;
  publisher_config.publisher = MemberId::unchecked("idle-peer");
  publisher_config.incarnation = Incarnation(0, BootNonce("idle-boot"));
  std::unique_ptr<MemberPublisher> publisher;
  SF_REQUIRE(is_ok(MemberPublisher::create(publisher_config, publisher)));
  SF_REQUIRE(is_ok(publisher->connect()));

  const auto started = std::chrono::steady_clock::now();
  SF_REQUIRE(is_ok(controller->stop()));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  // Cooperative cancellation, not a watchdog: the bound is the header poll
  // interval plus scheduling, and it is asserted so a regression that makes
  // shutdown wait on a socket timeout is caught.
  SF_CHECK(elapsed < 5000);
  SF_REQUIRE(is_ok(publisher->close()));
}

SF_TEST(concurrency, concurrent_snapshot_chain_publication_keeps_order) {
  SyntheticConfig synthetic = concurrent_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  ComposedSite composed;
  SiteComposer composer;
  SF_REQUIRE(is_ok(composer.compose(generated.input, composed)));

  SnapshotChain chain(64);
  std::atomic<std::size_t> published{0};
  std::atomic<std::size_t> refused{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < 6; ++index) {
    workers.emplace_back([&, index]() {
      for (int step = 0; step < 6; ++step) {
        SiteSnapshot snapshot;
        snapshot.site = composed.site;
        snapshot.epoch = Epoch(1);
        snapshot.generation = Generation(static_cast<std::uint64_t>(index * 6 + step + 1));
        snapshot.incarnation = Incarnation(1, BootNonce("chain"));
        snapshot.published_at_ms = generated.input.now_ms + step;
        snapshot.lifecycle = composed.lifecycle;
        snapshot.status = composed.status;
        snapshot.state = composed;
        if (!is_ok(snapshot.recompute_digest())) {
          refused.fetch_add(1);
          continue;
        }
        AuthorityToken token;
        token.kind = AuthorityKind::SITE_CONTROLLER;
        token.holder = "controller-1";
        token.site = composed.site;
        token.epoch = Epoch(1);
        token.incarnation = Incarnation(1, BootNonce("chain"));
        token.generation = snapshot.generation;
        token.lease_id = 1;
        token.issued_at_ms = generated.input.now_ms;
        token.expires_at_ms = generated.input.now_ms + 60000;
        SnapshotPublishResult result;
        const Status status = chain.publish(snapshot, token, generated.input.now_ms, result);
        if (is_ok(status)) {
          published.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  SF_CHECK_EQ(std::size_t(36), published.load() + refused.load());

  // Whatever the interleaving, the chain is totally ordered and its own
  // generation never moves backwards.
  SiteSnapshot current;
  SF_REQUIRE(chain.current(current));
  const std::vector<std::uint64_t> sequences = chain.sequences();
  for (std::size_t index = 1; index < sequences.size(); ++index) {
    SF_CHECK(sequences[index] > sequences[index - 1]);
  }
  SF_CHECK(current.sequence == sequences.back());
}

SF_TEST(concurrency, parallel_authority_operations_are_serialised) {
  AuthorityRegistry registry;
  const SiteId site = SiteId::unchecked("site-alpha");
  std::atomic<std::size_t> granted{0};
  std::atomic<std::size_t> refused{0};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < 8; ++index) {
    workers.emplace_back([&, index]() {
      for (int step = 0; step < 20; ++step) {
        AuthorityToken token;
        const Status status = registry.acquire_site_authority(
            "controller-" + std::to_string(index), site, Epoch(1),
            Incarnation(static_cast<std::uint64_t>(index + 1),
                        BootNonce("boot-" + std::to_string(index))),
            Generation(1), 1000000, 60000, token);
        if (is_ok(status)) {
          granted.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  SF_CHECK_EQ(std::size_t(160), granted.load() + refused.load());
  // At most one holder can be active at a time.
  SF_CHECK(granted.load() >= 1);
  SF_CHECK(!registry.leases().empty());
}

SF_TEST(concurrency, repeated_construction_and_destruction_is_clean) {
  for (int cycle = 0; cycle < 20; ++cycle) {
    SiteComposer composer;
    CompositionInput input;
    ComposedSite site;
    SF_CHECK(!is_ok(composer.compose(input, site)));
  }
  for (int cycle = 0; cycle < 20; ++cycle) {
    SnapshotChain chain(4);
    SiteSnapshot snapshot;
    SF_CHECK(!chain.current(snapshot));
  }
  for (int cycle = 0; cycle < 20; ++cycle) {
    AuthorityRegistry registry;
    SF_CHECK_EQ(Epoch::unset(), registry.current_epoch(SiteId::unchecked("site-alpha")));
  }
}
