// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// site_fabric_controller - the site controller process.
//
// It owns one site, holds the site lease, accepts member publications over
// loopback TCP, composes on every accepted publication and publishes a
// snapshot. It writes to a store only when --store is given, and it transmits
// nothing anywhere.
//
// It stops when --run-ms has elapsed or when the file named by --stop-file
// appears. There is no signal handler and no timeout: an operator interrupting
// the process is a hard kill, which is exactly the case the store is built to
// recover from.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "site_fabric/site_fabric.hpp"

namespace {

std::atomic<bool> g_stop{false};

struct Options {
  std::string site = "site-alpha";
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string store;
  std::string id = "controller-1";
  std::int64_t run_ms = 0;
  bool persistent = false;
  bool quiet = false;
  bool print_snapshots = false;
  std::vector<std::string> expect;
  std::string stop_file;
};

void usage() {
  std::fprintf(stderr, "usage: site_fabric_controller --port N [--host H] [--site S]\n");
  std::fprintf(stderr, "       [--id NAME] [--store PATH] [--expect KIND:ID]...\n");
  std::fprintf(stderr, "       [--stop-file PATH] [--run-ms N] [--print] [--quiet]\n");
}

bool parse(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        return nullptr;
      }
      return argv[++index];
    };
    if (argument == "--site") {
      const char* text = value("--site");
      if (text == nullptr) {
        return false;
      }
      options.site = text;
    } else if (argument == "--host") {
      const char* text = value("--host");
      if (text == nullptr) {
        return false;
      }
      options.host = text;
    } else if (argument == "--port") {
      const char* text = value("--port");
      if (text == nullptr) {
        return false;
      }
      options.port = static_cast<std::uint16_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--id") {
      const char* text = value("--id");
      if (text == nullptr) {
        return false;
      }
      options.id = text;
    } else if (argument == "--store") {
      const char* text = value("--store");
      if (text == nullptr) {
        return false;
      }
      options.store = text;
      options.persistent = true;
    } else if (argument == "--expect") {
      const char* text = value("--expect");
      if (text == nullptr) {
        return false;
      }
      options.expect.emplace_back(text);
    } else if (argument == "--stop-file") {
      const char* text = value("--stop-file");
      if (text == nullptr) {
        return false;
      }
      options.stop_file = text;
    } else if (argument == "--run-ms") {
      const char* text = value("--run-ms");
      if (text == nullptr) {
        return false;
      }
      options.run_ms = std::strtoll(text, nullptr, 10);
    } else if (argument == "--print") {
      options.print_snapshots = true;
    } else if (argument == "--quiet") {
      options.quiet = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    usage();
    return 2;
  }
  if (options.port == 0) {
    usage();
    return 2;
  }

  site_fabric::ControllerConfig config;
  config.site = site_fabric::SiteId::unchecked(options.site);
  config.id = site_fabric::ControllerId::unchecked(options.id);
  config.bind_host = options.host;
  config.bind_port = options.port;
  config.max_connections = 64;
  config.lease_ttl_ms = 60000;
  config.persistent = options.persistent;
  config.store.path = options.store;
  config.store.durable_commit = true;
  config.expectation.site = config.site;
  for (const auto& text : options.expect) {
    site_fabric::MemberDomainKey key;
    if (!site_fabric::MemberDomainKey::parse(text, key)) {
      std::fprintf(stderr, "invalid --expect: %s\n", text.c_str());
      return 2;
    }
    site_fabric::ExpectedMember member;
    member.domain = key;
    member.required = true;
    config.expectation.members.push_back(member);
  }

  std::unique_ptr<site_fabric::SiteController> controller;
  const site_fabric::Status created = site_fabric::SiteController::create(config, controller);
  if (!site_fabric::is_ok(created)) {
    std::fprintf(stderr, "controller refused: %s\n", site_fabric::to_string(created));
    return 3;
  }
  const site_fabric::Status started = controller->start();
  if (!site_fabric::is_ok(started)) {
    std::fprintf(stderr, "controller failed to start: %s\n", site_fabric::to_string(started));
    return 4;
  }

  if (!options.quiet) {
    std::printf("CONTROLLER_READY site=%s endpoint=%s epoch=%llu incarnation=%s\n",
                options.site.c_str(), controller->endpoint().c_str(),
                static_cast<unsigned long long>(controller->epoch().value()),
                controller->incarnation().to_string().c_str());
    std::fflush(stdout);
  }

  const auto started_at = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (options.run_ms > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started_at)
                               .count();
      if (elapsed >= options.run_ms) {
        break;
      }
    }
    if (!options.stop_file.empty()) {
      std::FILE* probe = std::fopen(options.stop_file.c_str(), "rb");
      if (probe != nullptr) {
        std::fclose(probe);
        break;
      }
    }
    if (options.print_snapshots) {
      site_fabric::SiteSnapshot snapshot;
      if (site_fabric::is_ok(controller->current_snapshot(snapshot))) {
        std::printf("SNAPSHOT sequence=%llu lifecycle=%s %s\n",
                    static_cast<unsigned long long>(snapshot.sequence),
                    site_fabric::to_string(snapshot.lifecycle),
                    snapshot.state.capacity.to_string().c_str());
        std::fflush(stdout);
      }
    }
  }

  site_fabric::SiteSnapshot snapshot;
  if (site_fabric::is_ok(controller->current_snapshot(snapshot))) {
    std::printf("CONTROLLER_FINAL sequence=%llu lifecycle=%s complete=%s members=%zu\n",
                static_cast<unsigned long long>(snapshot.sequence),
                site_fabric::to_string(snapshot.lifecycle),
                snapshot.complete ? "true" : "false", snapshot.state.members.size());
    std::printf("CONTROLLER_CAPACITY %s closes=%s\n", snapshot.state.capacity.to_string().c_str(),
                snapshot.state.capacity.closes_exactly ? "true" : "false");
  } else {
    std::printf("CONTROLLER_FINAL sequence=0 lifecycle=UNKNOWN complete=false members=0\n");
  }
  std::fflush(stdout);

  const site_fabric::Status stopped = controller->stop();
  if (!site_fabric::is_ok(stopped)) {
    std::fprintf(stderr, "stop returned %s\n", site_fabric::to_string(stopped));
    return 5;
  }
  return 0;
}
