// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <memory>
#include <string>
#include <vector>

#include "site_fabric/persistence.hpp"
#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

SyntheticConfig recovery_config() {
  SyntheticConfig config;
  config.clusters = 1;
  config.pods_per_cluster = 1;
  config.racks_per_pod = 2;
  config.shared_links = 1;
  config.gateways = 1;
  config.failure_domains = 1;
  config.maintenance_zones = 0;
  config.obligations = 0;
  return config;
}

ControllerConfig controller_config_for(const SiteExpectation& expectation,
                                       sftest::ManualClock& clock,
                                       const std::filesystem::path& store_path,
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
  config.persistent = true;
  config.store.path = store_path;
  config.store.durable_commit = true;
  return config;
}

}  // namespace

SF_TEST(recovery, recovered_members_are_stale_until_they_report_again) {
  sftest::TemporaryDirectory directory("recovery-stale");
  SyntheticConfig synthetic = recovery_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);
  const std::filesystem::path store_path = directory.file("site.sfstore");

  {
    std::unique_ptr<SiteController> controller;
    SF_REQUIRE(is_ok(SiteController::create(
        controller_config_for(generated.input.expectation, clock, store_path, "ctl-first"),
        controller)));
    SF_REQUIRE(is_ok(controller->start()));
    for (const auto& publication : generated.input.publications) {
      std::uint64_t assigned = 0;
      SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
          publication.domain, publication.incarnation, generated.input.now_ms, &assigned)));
      PublishResponse response;
      SF_REQUIRE(is_ok(controller->publish(publication.publisher, publication.incarnation,
                                           publication.declaration, publication.attempt,
                                           response)));
    }
    SiteSnapshot snapshot;
    SF_REQUIRE(is_ok(controller->current_snapshot(snapshot)));
    SF_CHECK_EQ(SiteLifecycle::CURRENT, snapshot.lifecycle);
    SF_REQUIRE(is_ok(controller->stop()));
  }

  // Reopen the store and start a second controller, exactly as a restart would.
  std::unique_ptr<SiteController> restarted;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, store_path, "ctl-second"),
      restarted)));
  {
    const Status started_status = restarted->start();
    if (!is_ok(started_status)) {
      std::error_code size_code;
      std::string detail = "restart start: " + std::string(to_string(started_status));
      detail += " path=" + store_path.string();
      detail += " exists=" + std::string(std::filesystem::exists(store_path) ? "yes" : "no");
      detail += " size=" + std::to_string(std::filesystem::file_size(store_path, size_code));
      detail += " | " + restarted->recovery().to_string();
      StoreContents probe_contents;
      const Status probe_status = SiteStore::read_file(store_path, probe_contents);
      detail += " | probe=" + std::string(to_string(probe_status));
      for (const auto& diagnostic : probe_contents.recovery.diagnostics) {
        detail += " | " + diagnostic;
      }
      sftest::record_failure(__FILE__, __LINE__, detail);
    }
    SF_REQUIRE(is_ok(started_status));
  }
  SF_CHECK_EQ(2, restarted->epoch().value());
  if (restarted->recovery().status != Status::OK) {
    std::string detail = "store recovery: " + restarted->recovery().to_string();
    StoreContents contents;
    if (is_ok(SiteStore::read_file(store_path, contents))) {
      for (const auto& diagnostic : contents.recovery.diagnostics) {
        detail += " | " + diagnostic;
      }
    }
    sftest::record_failure(__FILE__, __LINE__, detail);
  }
  SF_CHECK_EQ(Status::OK, restarted->recovery().status);
  SF_CHECK(restarted->recovery().usable());

  ComposedSite composed;
  SF_REQUIRE(is_ok(restarted->compose_now(composed)));
  // The declarations survived with valid digests, but they are history: every
  // recovered member is stale until it speaks again.
  for (const auto& member : composed.members) {
    SF_CHECK_EQ(MemberLifecycle::STALE, member.lifecycle);
    SF_CHECK_EQ(Status::EVIDENCE_STALE, member.status);
  }
  SF_CHECK_NE(SiteLifecycle::CURRENT, composed.lifecycle);

  // The restarted controller published at once, at the new epoch.
  SiteSnapshot snapshot;
  SF_REQUIRE(is_ok(restarted->current_snapshot(snapshot)));
  SF_CHECK_EQ(2, snapshot.epoch.value());

  // A member that reports live becomes current again.
  const MemberDomainDeclaration& declaration = generated.declarations.front();
  std::uint64_t assigned = 0;
  SF_REQUIRE(is_ok(restarted->authority().admit_member_incarnation(
      declaration.domain, Incarnation(0, BootNonce("member-fresh")), generated.input.now_ms,
      &assigned)));
  PublishResponse response;
  SF_REQUIRE(is_ok(restarted->publish(MemberId::unchecked("agent"),
                                      Incarnation(assigned, BootNonce("member-fresh")),
                                      declaration, PublishAttempt{1, 1}, response)));
  SF_CHECK_EQ(Status::ACCEPTED, response.status);
  SF_REQUIRE(is_ok(restarted->compose_now(composed)));
  const MemberDomainState* state = composed.find_member(declaration.domain);
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::CURRENT, state->lifecycle);
  SF_REQUIRE(is_ok(restarted->stop()));
}

SF_TEST(recovery, the_previous_epoch_is_fenced_after_a_restart) {
  sftest::TemporaryDirectory directory("recovery-fence");
  SyntheticConfig synthetic = recovery_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);
  const std::filesystem::path store_path = directory.file("site.sfstore");

  AuthorityToken first_token;
  {
    std::unique_ptr<SiteController> controller;
    SF_REQUIRE(is_ok(SiteController::create(
        controller_config_for(generated.input.expectation, clock, store_path, "ctl-first"),
        controller)));
    SF_REQUIRE(is_ok(controller->start()));
    SF_CHECK(controller->authority().current_controller(SiteId::unchecked("site-alpha"),
                                                        first_token));
    SF_REQUIRE(is_ok(controller->stop()));
  }

  std::unique_ptr<SiteController> restarted;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, store_path, "ctl-second"),
      restarted)));
  {
    const Status started_status = restarted->start();
    if (!is_ok(started_status)) {
      std::string detail = "restart start: " + std::string(to_string(started_status)) + " | " +
                           restarted->recovery().to_string();
      StoreContents probe_contents;
      if (is_ok(SiteStore::read_file(store_path, probe_contents))) {
        for (const auto& diagnostic : probe_contents.recovery.diagnostics) {
          detail += " | " + diagnostic;
        }
      }
      sftest::record_failure(__FILE__, __LINE__, detail);
    }
    SF_REQUIRE(is_ok(started_status));
  }
  SF_CHECK(restarted->epoch().value() > first_token.epoch.value());

  // The stale controller's epoch is refused by the restarted registry.
  SF_CHECK_EQ(Status::FENCED_EPOCH,
              restarted->authority().acquire_site_authority(
                  "controller-1", SiteId::unchecked("site-alpha"), first_token.epoch,
                  first_token.incarnation, Generation(first_token.generation.value() + 100),
                  generated.input.now_ms, 60000, first_token));
  SF_REQUIRE(is_ok(restarted->stop()));
}

SF_TEST(recovery, a_corrupt_store_refuses_to_start_rather_than_guessing) {
  sftest::TemporaryDirectory directory("recovery-corrupt");
  const std::filesystem::path store_path = directory.file("site.sfstore");

  SyntheticConfig synthetic = recovery_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  {
    std::unique_ptr<SiteController> controller;
    SF_REQUIRE(is_ok(SiteController::create(
        controller_config_for(generated.input.expectation, clock, store_path, "ctl-first"),
        controller)));
    SF_REQUIRE(is_ok(controller->start()));
    SF_REQUIRE(is_ok(controller->stop()));
  }

  // Corrupt the file header.
  {
    std::FILE* stream = std::fopen(store_path.string().c_str(), "rb+");
    SF_REQUIRE(stream != nullptr);
    const unsigned char junk = 0x5A;
    SF_REQUIRE(std::fwrite(&junk, 1, 1, stream) == 1);
    std::fclose(stream);
  }

  std::unique_ptr<SiteController> broken;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, store_path, "ctl-third"),
      broken)));
  const Status started = broken->start();
  SF_CHECK(!is_ok(started));
  SF_CHECK(!broken->running());
}

SF_TEST(recovery, snapshots_survive_a_restart_in_order) {
  sftest::TemporaryDirectory directory("recovery-history");
  SyntheticConfig synthetic = recovery_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);
  const std::filesystem::path store_path = directory.file("site.sfstore");

  std::vector<std::uint64_t> sequences;
  {
    std::unique_ptr<SiteController> controller;
    SF_REQUIRE(is_ok(SiteController::create(
        controller_config_for(generated.input.expectation, clock, store_path, "ctl-first"),
        controller)));
    SF_REQUIRE(is_ok(controller->start()));
    for (const auto& publication : generated.input.publications) {
      clock.advance(1000);
      std::uint64_t assigned = 0;
      SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
          publication.domain, publication.incarnation, clock.now(), &assigned)));
      PublishResponse response;
      SF_REQUIRE(is_ok(controller->publish(publication.publisher, publication.incarnation,
                                           publication.declaration, publication.attempt,
                                           response)));
    }
    SF_REQUIRE(is_ok(controller->snapshot_sequences(sequences)));
    SF_CHECK(sequences.size() > 1);
    SF_REQUIRE(is_ok(controller->stop()));
  }

  std::unique_ptr<SiteController> restarted;
  SF_REQUIRE(is_ok(SiteController::create(
      controller_config_for(generated.input.expectation, clock, store_path, "ctl-second"),
      restarted)));
  {
    const Status started_status = restarted->start();
    if (!is_ok(started_status)) {
      std::string detail = "restart start: " + std::string(to_string(started_status)) + " | " +
                           restarted->recovery().to_string();
      StoreContents probe_contents;
      if (is_ok(SiteStore::read_file(store_path, probe_contents))) {
        for (const auto& diagnostic : probe_contents.recovery.diagnostics) {
          detail += " | " + diagnostic;
        }
      }
      sftest::record_failure(__FILE__, __LINE__, detail);
    }
    SF_REQUIRE(is_ok(started_status));
  }
  SiteSnapshot restored;
  SF_REQUIRE(is_ok(restarted->current_snapshot(restored)));
  SF_CHECK(restored.sequence > sequences.back());
  SF_CHECK(restored.state.capacity.closes_exactly);
  SF_CHECK_EQ(generated.declarations.size(), restored.state.members.size());
  SF_REQUIRE(is_ok(restarted->stop()));
}
