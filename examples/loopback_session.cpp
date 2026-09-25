// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A controller and three member publishers talking over real loopback TCP.
//
// The transport is a real socket: this example would work unchanged if the
// publishers were separate processes on the same host, which is exactly what
// the multiprocess tests do. It is one process here only to keep the example
// short.

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"

int main() {
  site_fabric::SyntheticConfig config;
  config.seed = 7ULL;
  config.clusters = 1;
  config.pods_per_cluster = 1;
  config.racks_per_pod = 3;
  config.shared_links = 2;
  config.gateways = 1;
  config.failure_domains = 1;
  config.maintenance_zones = 0;
  config.obligations = 1;
  // The controller reads the real clock, so the fixture's evidence has to be
  // stamped against the real clock too. A fixture dated 2023 is stale by 2026,
  // and stale evidence is never authoritative.
  config.now_ms = site_fabric::system_now_ms();

  site_fabric::SyntheticSite generated;
  if (!site_fabric::is_ok(site_fabric::generate_synthetic_site(config, generated))) {
    std::printf("scenario refused\n");
    return 1;
  }

  site_fabric::ControllerConfig controller_config;
  controller_config.site = site_fabric::SiteId::unchecked("site-alpha");
  controller_config.id = site_fabric::ControllerId::unchecked("controller-1");
  controller_config.bind_host = "127.0.0.1";
  controller_config.bind_port = 0;  // an ephemeral port
  controller_config.expectation = generated.input.expectation;
  std::unique_ptr<site_fabric::SiteController> controller;
  if (!site_fabric::is_ok(
          site_fabric::SiteController::create(controller_config, controller))) {
    std::printf("controller refused\n");
    return 1;
  }
  if (!site_fabric::is_ok(controller->start())) {
    std::printf("controller failed to start\n");
    return 1;
  }
  std::printf("controller listening on %s (epoch %llu)\n", controller->endpoint().c_str(),
              static_cast<unsigned long long>(controller->epoch().value()));

  // The sessions are held open until the snapshot has been read: a member
  // domain is authoritative only while it holds a session, so closing early
  // would correctly demote every domain before the snapshot is taken.
  std::vector<std::unique_ptr<site_fabric::MemberPublisher>> publishers;
  for (std::size_t index = 0; index < generated.declarations.size(); ++index) {
    site_fabric::PublisherConfig publisher_config;
    publisher_config.host = "127.0.0.1";
    publisher_config.port = controller->port();
    publisher_config.site = controller_config.site;
    publisher_config.domain = generated.declarations[index].domain;
    publisher_config.publisher = site_fabric::MemberId::unchecked("agent-" + std::to_string(index));
    publisher_config.incarnation = site_fabric::Incarnation(
        0, site_fabric::BootNonce("agent-boot-" + std::to_string(index)));

    std::unique_ptr<site_fabric::MemberPublisher> publisher;
    if (!site_fabric::is_ok(
            site_fabric::MemberPublisher::create(publisher_config, publisher))) {
      std::printf("publisher refused\n");
      return 1;
    }
    if (!site_fabric::is_ok(publisher->connect())) {
      std::printf("connect failed for %s\n", publisher_config.domain.to_string().c_str());
      return 1;
    }
    site_fabric::PublishResponse response;
    const site_fabric::Status published = publisher->publish(
        generated.declarations[index], site_fabric::PublishAttempt{1, 1}, response);
    std::printf("  %-32s hello=%llu publish=%s\n",
                publisher_config.domain.to_string().c_str(),
                static_cast<unsigned long long>(publisher->session().assigned_sequence),
                site_fabric::to_string(published));
    publishers.push_back(std::move(publisher));
  }

  site_fabric::SiteSnapshot snapshot;
  if (site_fabric::is_ok(controller->current_snapshot(snapshot))) {
    std::printf("\nsnapshot %llu: %s\n", static_cast<unsigned long long>(snapshot.sequence),
                site_fabric::to_string(snapshot.lifecycle));
    std::printf("  %s\n", snapshot.state.capacity.to_string().c_str());
    std::printf("  closes exactly: %s\n", snapshot.state.capacity.closes_exactly ? "yes" : "no");
    for (const auto& attribution : snapshot.state.ownership) {
      std::printf("  %s\n", attribution.to_string().c_str());
    }
  }

  // Closing the sessions ends the authority those domains hold. The controller
  // notices on its next read, so the example waits for the change rather than
  // printing whatever happened to be there.
  for (auto& publisher : publishers) {
    (void)publisher->close();
  }
  site_fabric::SiteSnapshot demoted;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (!site_fabric::is_ok(controller->current_snapshot(demoted)) ||
        demoted.lifecycle != site_fabric::SiteLifecycle::CURRENT) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::printf("\nafter the sessions close the site is %s\n",
              site_fabric::to_string(demoted.lifecycle));
  for (const auto& member : demoted.state.members) {
    std::printf("  %s\n", member.to_string().c_str());
  }
  (void)controller->stop();
  return 0;
}
