// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

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

SyntheticConfig controller_config() {
  SyntheticConfig config;
  config.clusters = 1;
  config.pods_per_cluster = 2;
  config.racks_per_pod = 2;
  config.shared_links = 2;
  config.gateways = 1;
  config.failure_domains = 1;
  config.maintenance_zones = 0;
  config.obligations = 1;
  return config;
}

ControllerConfig make_controller_config(const SiteExpectation& expectation,
                                        sftest::ManualClock& clock,
                                        const std::string& incarnation_nonce) {
  ControllerConfig config;
  config.site = SiteId::unchecked("site-alpha");
  config.expectation = expectation;
  config.id = ControllerId::unchecked("controller-1");
  config.bind_host = "127.0.0.1";
  config.bind_port = 0;
  config.incarnation = Incarnation(1, BootNonce(incarnation_nonce));
  config.clock = clock.function();
  config.io_timeout_ms = 5000;
  config.lease_ttl_ms = 60000;
  config.compose_on_publish = true;
  return config;
}

}  // namespace

SF_TEST(controller, start_publish_and_stop) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-1"), controller)));
  SF_REQUIRE(is_ok(controller->start()));
  SF_CHECK(controller->running());
  SF_CHECK_NE(std::uint16_t(0), controller->port());
  SF_CHECK_EQ(Status::ALREADY_EXISTS, controller->start());

  for (const auto& publication : generated.input.publications) {
    std::uint64_t assigned = 0;
    SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
        publication.domain, publication.incarnation, generated.input.now_ms, &assigned)));
    PublishResponse response;
    SF_CHECK_EQ(Status::ACCEPTED,
                controller->publish(publication.publisher, publication.incarnation,
                                    publication.declaration, publication.attempt, response));
    SF_CHECK(response.acceptance_sequence != 0);
  }

  SiteSnapshot snapshot;
  SF_REQUIRE(is_ok(controller->current_snapshot(snapshot)));
  SF_CHECK_EQ(SiteLifecycle::CURRENT, snapshot.lifecycle);
  SF_CHECK(snapshot.complete);
  // The controller publishes once on start, so the first member publication is
  // the second snapshot.
  SF_CHECK(snapshot.sequence >= 2);
  SF_CHECK_EQ(Status::CURRENT, snapshot.state.status);

  const ControllerStats stats = controller->stats();
  SF_CHECK_EQ(generated.input.publications.size(), stats.publications_accepted);

  SF_REQUIRE(is_ok(controller->stop()));
  SF_CHECK(!controller->running());
  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(controller, member_publisher_over_loopback_tcp) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-2"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  std::vector<std::unique_ptr<MemberPublisher>> publishers;
  for (std::size_t index = 0; index < generated.declarations.size(); ++index) {
    PublisherConfig publisher_config;
    publisher_config.host = "127.0.0.1";
    publisher_config.port = controller->port();
    publisher_config.site = SiteId::unchecked("site-alpha");
    publisher_config.domain = generated.declarations[index].domain;
    publisher_config.publisher = MemberId::unchecked("member-agent-" + std::to_string(index));
    publisher_config.incarnation =
        Incarnation(0, BootNonce("member-boot-" + std::to_string(index)));
    publisher_config.io_timeout_ms = 5000;

    std::unique_ptr<MemberPublisher> publisher;
    SF_REQUIRE(is_ok(MemberPublisher::create(publisher_config, publisher)));
    SF_REQUIRE(is_ok(publisher->connect()));
    SF_CHECK(publisher->session().established);
    SF_CHECK(publisher->session().assigned_sequence != 0);

    std::uint64_t echoed = 0;
    SF_REQUIRE(is_ok(publisher->ping(12345, echoed)));
    SF_CHECK_EQ(std::uint64_t(12345), echoed);

    PublishResponse response;
    SF_CHECK_EQ(Status::ACCEPTED, publisher->publish(generated.declarations[index],
                                                     PublishAttempt{1, 1}, response));

    HeartbeatResponse heartbeat;
    SF_REQUIRE(is_ok(publisher->heartbeat(heartbeat)));
    SF_CHECK(heartbeat.site_sequence != 0);

    FetchSiteResponse fetched;
    SF_REQUIRE(is_ok(publisher->fetch_site(0, fetched)));
    SF_CHECK(fetched.has_snapshot);
    publishers.push_back(std::move(publisher));
  }

  SiteSnapshot snapshot;
  SF_REQUIRE(is_ok(controller->current_snapshot(snapshot)));
  SF_CHECK_EQ(SiteLifecycle::CURRENT, snapshot.lifecycle);

  // Closing the sessions ends the authority those domains hold, so the site
  // stops reporting them as current. Nothing is retracted silently.
  for (auto& publisher : publishers) {
    SF_REQUIRE(is_ok(publisher->close()));
  }
  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(controller, a_closed_session_tombstones_the_incarnation) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-3"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  const MemberDomainDeclaration& declaration = generated.declarations.front();
  const Incarnation incarnation(0, BootNonce("member-boot-replay"));

  PublisherConfig publisher_config;
  publisher_config.host = "127.0.0.1";
  publisher_config.port = controller->port();
  publisher_config.site = SiteId::unchecked("site-alpha");
  publisher_config.domain = declaration.domain;
  publisher_config.publisher = MemberId::unchecked("member-agent");
  publisher_config.incarnation = incarnation;

  std::unique_ptr<MemberPublisher> first;
  SF_REQUIRE(is_ok(MemberPublisher::create(publisher_config, first)));
  SF_REQUIRE(is_ok(first->connect()));
  std::uint64_t assigned = first->session().assigned_sequence;
  SF_CHECK(assigned != 0);
  SF_REQUIRE(is_ok(first->close()));

  // The controller learns the session ended when it observes the close. The
  // tombstone is written then, so wait for it rather than assuming.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!controller->authority().member_incarnation_live(declaration.domain,
                                                         Incarnation(assigned, incarnation.boot))) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  SF_CHECK(!controller->authority().member_incarnation_live(
      declaration.domain, Incarnation(assigned, incarnation.boot)));

  // The same process lifetime replaying the same incarnation is refused.
  std::uint64_t again = 0;
  SF_CHECK_EQ(Status::FENCED_INCARNATION,
              controller->authority().admit_member_incarnation(
                  declaration.domain, Incarnation(assigned, incarnation.boot),
                  generated.input.now_ms, &again));

  // A genuinely new process is admitted with the next sequence.
  std::uint64_t next = 0;
  SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
      declaration.domain, Incarnation(0, BootNonce("member-boot-fresh")),
      generated.input.now_ms, &next)));
  SF_CHECK_EQ(assigned + 1, next);
  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(controller, fenced_publications_are_refused) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-4"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  const MemberDomainDeclaration* rack_declaration = nullptr;
  for (const auto& candidate : generated.declarations) {
    if (!candidate.racks.empty()) {
      rack_declaration = &candidate;
      break;
    }
  }
  SF_REQUIRE(rack_declaration != nullptr);
  const MemberDomainDeclaration& declaration = *rack_declaration;
  const Incarnation incarnation(0, BootNonce("boot-fenced"));
  std::uint64_t assigned = 0;
  SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
      declaration.domain, incarnation, generated.input.now_ms, &assigned)));
  const Incarnation live(assigned, incarnation.boot);

  PublishResponse response;
  SF_REQUIRE(is_ok(controller->publish(MemberId::unchecked("agent"), live, declaration,
                                       PublishAttempt{1, 1}, response)));
  SF_CHECK_EQ(Status::ACCEPTED, response.status);

  // Publishing before the hello.
  // A different process lifetime claiming the sequence this domain was issued
  // is fenced, and told exactly that.
  PublishResponse foreign;
  SF_CHECK_EQ(Status::FENCED_INCARNATION,
              controller->publish(MemberId::unchecked("agent"),
                                  Incarnation(assigned, BootNonce("other")), declaration,
                                  PublishAttempt{2, 1}, foreign));

  // Replaying the exact attempt.
  PublishResponse replayed;
  SF_CHECK_EQ(Status::FENCED_ATTEMPT,
              controller->publish(MemberId::unchecked("agent"), live, declaration,
                                  PublishAttempt{1, 1}, replayed));

  // A lower generation.
  MemberDomainDeclaration older = declaration;
  older.generation = Generation::unset();
  PublishResponse stale;
  SF_CHECK_EQ(Status::INVALID,
              controller->publish(MemberId::unchecked("agent"), live, older,
                                  PublishAttempt{3, 1}, stale));

  // A declaration whose digest does not match its content.
  MemberDomainDeclaration tampered = declaration;
  SF_REQUIRE(!tampered.racks.empty());
  tampered.racks.front().local_capacity.internal = CapacityValue(1);
  PublishResponse mismatched;
  SF_CHECK_EQ(Status::DIGEST_MISMATCH_DECLARATION,
              controller->publish(MemberId::unchecked("agent"), live, tampered,
                                  PublishAttempt{3, 1}, mismatched));

  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(controller, changed_since_reports_only_newer_publications) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-5"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  std::uint64_t mark = 0;
  for (const auto& publication : generated.input.publications) {
    std::uint64_t assigned = 0;
    SF_REQUIRE(is_ok(controller->authority().admit_member_incarnation(
        publication.domain, publication.incarnation, generated.input.now_ms, &assigned)));
    PublishResponse response;
    SF_REQUIRE(is_ok(controller->publish(publication.publisher, publication.incarnation,
                                         publication.declaration, publication.attempt,
                                         response)));
    mark = response.acceptance_sequence;
  }

  std::vector<MemberDomainKey> changed;
  SF_REQUIRE(is_ok(controller->changed_since(mark, changed)));
  SF_CHECK(changed.empty());
  SF_REQUIRE(is_ok(controller->changed_since(0, changed)));
  SF_CHECK_EQ(generated.input.publications.size(), changed.size());
  SF_REQUIRE(is_ok(controller->stop()));
}

SF_TEST(controller, a_started_controller_publishes_and_a_stopped_one_keeps_history) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-6"), controller)));
  SF_REQUIRE(is_ok(controller->start()));
  SiteSnapshot snapshot;
  SF_REQUIRE(is_ok(controller->current_snapshot(snapshot)));
  SF_CHECK_EQ(std::uint64_t(1), snapshot.sequence);
  // Nobody has reported yet, so the site is not current. It is not reported as
  // missing either: there is simply nothing to be current about.
  SF_CHECK_NE(SiteLifecycle::CURRENT, snapshot.lifecycle);
  SF_CHECK_EQ(generated.input.expectation.members.size(), snapshot.state.members.size());
  for (const auto& member : snapshot.state.members) {
    SF_CHECK_EQ(MemberLifecycle::ABSENT, member.lifecycle);
  }
  SF_REQUIRE(is_ok(controller->stop()));
  // The last snapshot stays readable after the controller stops; it is history.
  SiteSnapshot after;
  SF_REQUIRE(is_ok(controller->current_snapshot(after)));
  SF_CHECK_EQ(std::uint64_t(1), after.sequence);
}

SF_TEST(controller, repeated_start_and_stop_is_stable) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-7"), controller)));
  for (int cycle = 0; cycle < 5; ++cycle) {
    SF_REQUIRE(is_ok(controller->start()));
    SF_CHECK(controller->running());
    SF_REQUIRE(is_ok(controller->stop()));
    SF_CHECK(!controller->running());
    SF_REQUIRE(is_ok(controller->stop()));
  }
}

SF_TEST(controller, serve_returns_when_the_flag_is_set) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-8"), controller)));
  SF_REQUIRE(is_ok(controller->start()));

  std::atomic<bool> stop_flag{false};
  std::thread stopper([&stop_flag]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_flag.store(true);
  });
  SF_REQUIRE(is_ok(controller->serve(stop_flag)));
  stopper.join();
  SF_CHECK(!controller->running());
}

SF_TEST(controller, revoke_member_makes_the_site_incomplete) {
  SyntheticConfig synthetic = controller_config();
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(synthetic, generated)));
  sftest::ManualClock clock(generated.input.now_ms);

  std::unique_ptr<SiteController> controller;
  SF_REQUIRE(is_ok(SiteController::create(
      make_controller_config(generated.input.expectation, clock, "ctl-boot-9"), controller)));
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
  SiteSnapshot before;
  SF_REQUIRE(is_ok(controller->current_snapshot(before)));
  SF_CHECK_EQ(SiteLifecycle::CURRENT, before.lifecycle);

  const MemberDomainKey revoked_domain = generated.declarations.front().domain;
  SF_REQUIRE(is_ok(controller->revoke_member(revoked_domain, "operator")));
  SiteSnapshot after;
  SF_REQUIRE(is_ok(controller->current_snapshot(after)));
  SF_CHECK_EQ(SiteLifecycle::INCOMPLETE, after.lifecycle);
  SF_CHECK(after.sequence > before.sequence);
  SF_REQUIRE(is_ok(controller->stop()));
}
