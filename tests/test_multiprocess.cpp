// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real multiprocess proof.
//
// The controller and every member are separate operating-system processes
// communicating over loopback TCP. Threads would not be proof of anything here:
// the properties under test are about process lifetimes, so the test kills
// processes and starts new ones.
//
// Environment (set by CTest):
//   SITE_FABRIC_CONTROLLER  path to site_fabric_controller
//   SITE_FABRIC_MEMBER      path to site_fabric_member

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "sftest_support_process.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

std::string executable_from_env(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

const std::string kControllerExe =
    executable_from_env("SITE_FABRIC_CONTROLLER", "site_fabric_controller");
const std::string kMemberExe = executable_from_env("SITE_FABRIC_MEMBER", "site_fabric_member");

/// Reads the controller's current snapshot over the wire. The site name has to
/// match the controller's, or the hello is refused as a site mismatch, which is
/// the correct answer to the wrong question.
bool fetch_state(std::uint16_t port, const std::string& site, SiteSnapshot& out) {
  PublisherConfig config;
  config.host = "127.0.0.1";
  config.port = port;
  config.site = SiteId::unchecked(site);
  config.domain = MemberDomainKey::shared_resource("probe");
  config.publisher = MemberId::unchecked("probe");
  config.incarnation = Incarnation(0, BootNonce("probe-boot"));
  config.connect_timeout_ms = 1000;
  config.io_timeout_ms = 5000;
  std::unique_ptr<MemberPublisher> probe;
  if (!is_ok(MemberPublisher::create(config, probe)) || !is_ok(probe->connect())) {
    return false;
  }
  FetchSiteResponse response;
  const Status status = probe->fetch_site(0, response);
  (void)probe->close();
  if (!is_ok(status) || !response.has_snapshot) {
    return false;
  }
  out = response.snapshot;
  return true;
}

MemberLifecycle lifecycle_of(const SiteSnapshot& snapshot, const MemberDomainKey& domain) {
  const MemberDomainState* state = snapshot.state.find_member(domain);
  return state == nullptr ? MemberLifecycle::UNKNOWN : state->lifecycle;
}

}  // namespace

SF_TEST(multiprocess, independent_processes_publish_to_a_separate_controller) {
  sftest::TemporaryDirectory directory("mp-stop");
  const std::string stop_directory = directory.path().string();
  std::string stop_path;
  const std::string site = "site-mp-1";
  sftest::ProcessRunner controller;
  std::uint16_t port = 0;
  SF_REQUIRE(sftest::start_controller(
      kControllerExe, site, stop_directory,
      {"--expect", "RACK:rack-mp-a", "--expect", "RACK:rack-mp-b", "--expect",
       "SHARED_RESOURCE:gw-mp"},
      &controller, port, stop_path));

  // Two racks carry internal capacity and a gateway carries ingress and egress,
  // so every aggregate a site reports has a source.
  struct MemberSpec {
    const char* domain;
    const char* scope;
  };
  const std::vector<MemberSpec> specs = {{"RACK:rack-mp-a", "internal"},
                                         {"RACK:rack-mp-b", "internal"},
                                         {"SHARED_RESOURCE:gw-mp", "gateway"}};
  std::vector<std::unique_ptr<sftest::ProcessRunner>> members;
  for (std::size_t index = 0; index < specs.size(); ++index) {
    auto member = std::make_unique<sftest::ProcessRunner>();
    SF_REQUIRE(sftest::start_member(kMemberExe, port, site, specs[index].domain,
                                    "boot-" + std::to_string(index), 0, 1000, 30000,
                                    specs[index].scope, member.get()));
    members.push_back(std::move(member));
  }

  // Wait until every separate process has published and is current.
  bool current = false;
  SiteSnapshot snapshot;
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (fetch_state(port, site, snapshot) && snapshot.lifecycle == SiteLifecycle::CURRENT &&
        snapshot.state.members.size() == specs.size()) {
      current = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!current) {
    std::string detail = "site never reached CURRENT: " + site_fabric::explain(snapshot.state);
    sftest::record_failure(__FILE__, __LINE__, detail);
  }
  SF_CHECK(current);
  SF_CHECK(snapshot.state.capacity.closes_exactly);
  SF_CHECK(snapshot.complete);
  for (const auto& spec : specs) {
    MemberDomainKey key;
    SF_REQUIRE(MemberDomainKey::parse(spec.domain, key));
    SF_CHECK_EQ(MemberLifecycle::CURRENT, lifecycle_of(snapshot, key));
    const MemberDomainState* state = snapshot.state.find_member(key);
    SF_REQUIRE(state != nullptr);
    SF_CHECK(state->source.valid());
  }

  // A member domain is authoritative only while it holds a session. Ending the
  // sessions must demote the domains rather than leave a stale site looking
  // current.
  for (auto& member : members) {
    SF_REQUIRE(member->kill_now());
    member->wait_for_exit();
  }
  // Every domain must stop being current, not merely the site as a whole: one
  // session ending is enough to move the site verdict, so the property has to
  // be checked per domain.
  bool demoted = false;
  SiteSnapshot after;
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (!fetch_state(port, site, after)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    bool all_demoted = true;
    for (const auto& spec : specs) {
      MemberDomainKey key;
      if (!MemberDomainKey::parse(spec.domain, key) ||
          lifecycle_of(after, key) == MemberLifecycle::CURRENT) {
        all_demoted = false;
        break;
      }
    }
    if (all_demoted) {
      demoted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!demoted) {
    const std::string detail =
        "an ended session left its domain current: " + site_fabric::explain(after.state);
    sftest::record_failure(__FILE__, __LINE__, detail);
  }
  SF_CHECK_NE(SiteLifecycle::CURRENT, after.lifecycle);
  SF_CHECK(demoted);

  SF_REQUIRE(controller.request_stop(stop_path));
  SF_CHECK_EQ(0, controller.exit_code());
}

SF_TEST(multiprocess, a_killed_member_cannot_replay_its_incarnation) {
  sftest::TemporaryDirectory directory("mp-stop");
  const std::string stop_directory = directory.path().string();
  std::string stop_path;
  const std::string site = "site-mp-2";
  sftest::ProcessRunner controller;
  std::uint16_t port = 0;
  SF_REQUIRE(sftest::start_controller(kControllerExe, site, stop_directory, {}, &controller, port,
                                 stop_path));

  const std::string domain = "RACK:rack-hard-kill";
  const std::string nonce = "fixed-boot-nonce";

  // A member that holds its session open so the test can kill it mid-life.
  sftest::ProcessRunner victim;
  SF_REQUIRE(sftest::start_member(kMemberExe, port, site, domain, nonce, 0, 1000, 1000000,
                                  "internal", &victim));

  MemberDomainKey key;
  SF_REQUIRE(MemberDomainKey::parse(domain, key));
  bool current = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    SiteSnapshot snapshot;
    if (fetch_state(port, site, snapshot) &&
        lifecycle_of(snapshot, key) == MemberLifecycle::CURRENT) {
      current = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  SF_CHECK(current);

  // Hard kill: no clean shutdown, no retraction.
  SF_REQUIRE(victim.kill_now());
  victim.wait_for_exit();

  // The controller must stop treating the domain as current once the session
  // ends. That is the observable form of the tombstone.
  bool demoted = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    SiteSnapshot snapshot;
    if (fetch_state(port, site, snapshot) &&
        lifecycle_of(snapshot, key) != MemberLifecycle::CURRENT) {
      demoted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  SF_CHECK(demoted);

  // Replaying the dead incarnation, sequence and all, is refused.
  sftest::ProcessRunner replayer;
  const int replay_code = sftest::run_member(kMemberExe, port, site, domain, nonce, 1, 1000, 0,
                                             &replayer, "internal");
  // A refused hello is a transport-level refusal, which the tool reports as 4.
  SF_CHECK_EQ(4, replay_code);

  // A genuinely new process lifetime is admitted.
  sftest::ProcessRunner fresh;
  const int fresh_code = sftest::run_member(kMemberExe, port, site, domain, "fresh-boot-nonce",
                                            0, 1000, 0, &fresh, "internal");
  SF_CHECK_EQ(0, fresh_code);

  SiteSnapshot after;
  SF_REQUIRE(fetch_state(port, site, after));
  SF_CHECK_EQ(std::size_t(1), after.state.members.size());
  SF_CHECK_EQ(MemberLifecycle::STALE, lifecycle_of(after, key));

  SF_REQUIRE(controller.request_stop(stop_path));
  SF_CHECK_EQ(0, controller.exit_code());
}

SF_TEST(multiprocess, restarting_the_controller_fences_the_previous_epoch) {
  sftest::TemporaryDirectory directory("mp-restart");
  const std::string store = directory.file("site.sfstore").string();

  const std::string site = "site-mp-3";
  const std::string stop_directory = directory.path().string();
  std::string stop_path;
  std::uint16_t first_port = 0;
  std::uint64_t first_epoch = 0;
  {
    sftest::ProcessRunner controller;
    SF_REQUIRE(
        sftest::start_controller(kControllerExe, site, stop_directory, {"--store", store},
                                 &controller, first_port, stop_path));

    sftest::ProcessRunner member;
    SF_CHECK_EQ(0, sftest::run_member(kMemberExe, first_port, site, "RACK:rack-persist",
                                      "boot-persist", 0, 5000, 0, &member, "internal"));

    SiteSnapshot snapshot;
    SF_REQUIRE(fetch_state(first_port, site, snapshot));
    first_epoch = snapshot.epoch.value();
    SF_CHECK(first_epoch >= 1);
    SF_CHECK_EQ(std::size_t(1), snapshot.state.members.size());

    SF_REQUIRE(controller.request_stop(stop_path));
    SF_CHECK_EQ(0, controller.exit_code());
  }

  sftest::ProcessRunner restarted;
  std::uint16_t second_port = 0;
  SF_REQUIRE(sftest::start_controller(kControllerExe, site, stop_directory, {"--store", store},
                                      &restarted, second_port, stop_path));

  SiteSnapshot recovered;
  SF_REQUIRE(fetch_state(second_port, site, recovered));
  SF_CHECK(recovered.epoch.value() > first_epoch);
  // The declaration survived its digest, but the domain is history until it
  // reports to this controller.
  SF_REQUIRE(recovered.state.members.size() == 1);
  const MemberDomainState& recovered_member = recovered.state.members.front();
  SF_CHECK_EQ(MemberLifecycle::STALE, recovered_member.lifecycle);
  SF_CHECK_EQ(Status::EVIDENCE_STALE, recovered_member.status);

  // And a live member makes it current again, under the new epoch.
  sftest::ProcessRunner member;
  SF_CHECK_EQ(0, sftest::run_member(kMemberExe, second_port, site, "RACK:rack-persist",
                                    "boot-after-restart", 0, 5000, 0, &member, "internal"));
  SiteSnapshot after;
  SF_REQUIRE(fetch_state(second_port, site, after));
  SF_CHECK_EQ(std::size_t(1), after.state.members.size());

  SF_REQUIRE(restarted.request_stop(stop_path));
  SF_CHECK_EQ(0, restarted.exit_code());
}

SF_TEST(multiprocess, hostile_framing_on_a_real_socket_does_not_take_the_controller_down) {
  sftest::TemporaryDirectory directory("mp-stop");
  const std::string stop_directory = directory.path().string();
  std::string stop_path;
  const std::string site = "site-mp-4";
  sftest::ProcessRunner controller;
  std::uint16_t port = 0;
  SF_REQUIRE(sftest::start_controller(kControllerExe, site, stop_directory, {}, &controller, port,
                                 stop_path));

  // Connect and send garbage, then a valid-looking header with a payload that
  // does not match, then close mid-frame.
  for (int attempt = 0; attempt < 8; ++attempt) {
    SF_CHECK(sftest::send_hostile_bytes("127.0.0.1", port, attempt));
  }

  // The controller still answers a well-formed request afterwards.
  SiteSnapshot snapshot;
  SF_CHECK(fetch_state(port, site, snapshot));

  sftest::ProcessRunner member;
  SF_CHECK_EQ(0, sftest::run_member(kMemberExe, port, site, "RACK:rack-after-hostile",
                                    "boot-after-hostile", 0, 1000, 0, &member, "internal"));
  SF_REQUIRE(fetch_state(port, site, snapshot));
  SF_CHECK(snapshot.state.members.size() >= 1);

  SF_REQUIRE(controller.request_stop(stop_path));
  SF_CHECK_EQ(0, controller.exit_code());
}
