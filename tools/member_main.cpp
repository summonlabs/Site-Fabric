// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// site_fabric_member - a member-domain publisher.
//
// The tool builds a declaration for one member domain from its arguments,
// connects to a controller over loopback TCP, completes the hello handshake and
// publishes. It is the same code path the multiprocess tests exercise, and it
// is a real, separate operating-system process when they run it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "site_fabric/site_fabric.hpp"

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string site = "site-alpha";
  std::string domain = "RACK:rack-0";
  std::string publisher = "member";
  std::string boot_nonce;
  std::uint64_t capacity_bps = 1000000000ULL;
  std::int64_t hold_ms = 0;
  std::int64_t ttl_ms = 60000;
  bool external = true;
  std::string scope = "ingress";
  bool quiet = false;
  std::uint32_t schema_major = site_fabric::kModelSchemaMajor;
  std::uint64_t generation = 1;
  std::uint64_t sequence = 0;
};

void usage() {
  std::fprintf(stderr, "usage: site_fabric_member --port N [--host H] [--site S] --domain KIND:ID\n");
  std::fprintf(stderr, "       [--publisher NAME] [--boot-nonce NONCE] [--capacity-bps N]\n");
  std::fprintf(stderr, "       [--hold-ms N] [--ttl-ms N] [--internal]\n");
  std::fprintf(stderr, "       [--scope ingress|egress|internal|gateway] [--schema-major N]\n");
  std::fprintf(stderr, "exit codes: 0 published, 2 usage, 3 refused locally,\n");
  std::fprintf(stderr, "            4 transport or handshake refused, 5 publication refused\n");
  std::fprintf(stderr, "       [--generation N] [--sequence N] [--quiet]\n");
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
    if (argument == "--host") {
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
    } else if (argument == "--site") {
      const char* text = value("--site");
      if (text == nullptr) {
        return false;
      }
      options.site = text;
    } else if (argument == "--domain") {
      const char* text = value("--domain");
      if (text == nullptr) {
        return false;
      }
      options.domain = text;
    } else if (argument == "--publisher") {
      const char* text = value("--publisher");
      if (text == nullptr) {
        return false;
      }
      options.publisher = text;
    } else if (argument == "--boot-nonce") {
      const char* text = value("--boot-nonce");
      if (text == nullptr) {
        return false;
      }
      options.boot_nonce = text;
    } else if (argument == "--capacity-bps") {
      const char* text = value("--capacity-bps");
      if (text == nullptr) {
        return false;
      }
      options.capacity_bps = std::strtoull(text, nullptr, 10);
    } else if (argument == "--hold-ms") {
      const char* text = value("--hold-ms");
      if (text == nullptr) {
        return false;
      }
      options.hold_ms = std::strtoll(text, nullptr, 10);
    } else if (argument == "--ttl-ms") {
      const char* text = value("--ttl-ms");
      if (text == nullptr) {
        return false;
      }
      options.ttl_ms = std::strtoll(text, nullptr, 10);
    } else if (argument == "--schema-major") {
      const char* text = value("--schema-major");
      if (text == nullptr) {
        return false;
      }
      options.schema_major = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
    } else if (argument == "--generation") {
      const char* text = value("--generation");
      if (text == nullptr) {
        return false;
      }
      options.generation = std::strtoull(text, nullptr, 10);
    } else if (argument == "--sequence") {
      const char* text = value("--sequence");
      if (text == nullptr) {
        return false;
      }
      options.sequence = std::strtoull(text, nullptr, 10);
    } else if (argument == "--scope") {
      const char* text = value("--scope");
      if (text == nullptr) {
        return false;
      }
      options.scope = text;
    } else if (argument == "--internal") {
      options.external = false;
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

  // "internal" is a statement about which aggregate the capacity feeds, so it
  // also decides which channels the declaration reports.
  if (options.scope == "internal") {
    options.external = false;
  }

  site_fabric::MemberDomainKey key;
  if (!site_fabric::MemberDomainKey::parse(options.domain, key)) {
    std::fprintf(stderr, "invalid --domain: %s\n", options.domain.c_str());
    return 2;
  }

  site_fabric::Incarnation incarnation;
  incarnation.sequence = options.sequence;
  incarnation.boot = site_fabric::BootNonce(
      options.boot_nonce.empty()
          ? ("member-" + std::to_string(static_cast<unsigned long long>(
                              std::chrono::steady_clock::now().time_since_epoch().count())))
          : options.boot_nonce);

  site_fabric::MemberDomainDeclaration declaration;
  declaration.domain = key;
  declaration.site = site_fabric::SiteId::unchecked(options.site);
  declaration.generation = site_fabric::Generation(options.generation);
  declaration.observed_epoch = site_fabric::Epoch::initial();
  declaration.schema =
      site_fabric::SchemaVersion{options.schema_major, site_fabric::kModelSchemaMinor};

  site_fabric::Evidence evidence;
  evidence.provenance = site_fabric::Provenance::REPORTED;
  evidence.observed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
  evidence.ttl_ms = static_cast<std::uint64_t>(options.ttl_ms);
  declaration.evidence = evidence;

  const site_fabric::CapacityVector capacity =
      options.external
          ? site_fabric::CapacityVector(site_fabric::CapacityValue(options.capacity_bps),
                                        site_fabric::CapacityValue(options.capacity_bps),
                                        site_fabric::CapacityValue::unknown())
          : site_fabric::CapacityVector(site_fabric::CapacityValue::unknown(),
                                        site_fabric::CapacityValue::unknown(),
                                        site_fabric::CapacityValue(options.capacity_bps));

  switch (key.kind) {
    case site_fabric::MemberDomainKind::RACK: {
      site_fabric::RackClaim claim;
      claim.id = site_fabric::RackId::unchecked(key.id);
      claim.generation = declaration.generation;
      claim.local_capacity = capacity;
      claim.uplink = site_fabric::ConnectivityState::UP;
      claim.evidence = evidence;
      declaration.racks.push_back(std::move(claim));
      break;
    }
    case site_fabric::MemberDomainKind::POD: {
      site_fabric::PodClaim claim;
      claim.id = site_fabric::PodId::unchecked(key.id);
      claim.generation = declaration.generation;
      claim.evidence = evidence;
      declaration.pods.push_back(std::move(claim));
      break;
    }
    case site_fabric::MemberDomainKind::CLUSTER: {
      site_fabric::ClusterClaim claim;
      claim.id = site_fabric::ClusterId::unchecked(key.id);
      claim.generation = declaration.generation;
      claim.evidence = evidence;
      declaration.clusters.push_back(std::move(claim));
      break;
    }
    case site_fabric::MemberDomainKind::SHARED_RESOURCE: {
      site_fabric::CapacityContribution pool;
      pool.owner = site_fabric::ResourceKey::capacity_pool(key.id);
      pool.scope = options.external ? site_fabric::CapacityScope::SITE_INGRESS
                                    : site_fabric::CapacityScope::SITE_INTERNAL;
      pool.generation = declaration.generation;
      pool.capacity = capacity;
      pool.evidence = evidence;
      declaration.capacity.push_back(std::move(pool));
      break;
    }
    default:
      std::fprintf(stderr, "unsupported domain kind\n");
      return 2;
  }

  // A shared-resource domain may declare a gateway instead of a bare pool. A
  // gateway feeds both the ingress and the egress aggregate, which is what a
  // site with external connectivity actually has.
  if (key.kind == site_fabric::MemberDomainKind::SHARED_RESOURCE &&
      options.scope == "gateway") {
    declaration.capacity.clear();
    site_fabric::GatewayClaim gateway;
    gateway.id = site_fabric::GatewayId::unchecked(key.id);
    gateway.generation = declaration.generation;
    gateway.capacity = capacity;
    gateway.state = site_fabric::ConnectivityState::UP;
    gateway.evidence = evidence;
    declaration.gateways.push_back(std::move(gateway));
  } else if (key.kind == site_fabric::MemberDomainKind::SHARED_RESOURCE) {
    declaration.capacity.clear();
    site_fabric::CapacityContribution pool;
    pool.owner = site_fabric::ResourceKey::capacity_pool(key.id);
    pool.scope = options.scope == "egress"    ? site_fabric::CapacityScope::SITE_EGRESS
                 : options.scope == "internal" ? site_fabric::CapacityScope::SITE_INTERNAL
                                               : site_fabric::CapacityScope::SITE_INGRESS;
    pool.generation = declaration.generation;
    pool.capacity = capacity;
    pool.evidence = evidence;
    declaration.capacity.push_back(std::move(pool));
  }

  const site_fabric::Status sealed = declaration.seal();
  if (!site_fabric::is_ok(sealed)) {
    std::fprintf(stderr, "declaration refused: %s\n", site_fabric::to_string(sealed));
    return 3;
  }

  site_fabric::PublisherConfig config;
  config.host = options.host;
  config.port = options.port;
  config.site = site_fabric::SiteId::unchecked(options.site);
  config.domain = key;
  config.publisher = site_fabric::MemberId::unchecked(options.publisher);
  config.incarnation = incarnation;
  config.schema = declaration.schema;

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
  if (!options.quiet) {
    std::printf("MEMBER_HELLO domain=%s status=%s sequence=%llu\n", options.domain.c_str(),
                site_fabric::to_string(publisher->session().status),
                static_cast<unsigned long long>(publisher->session().assigned_sequence));
    std::fflush(stdout);
  }

  site_fabric::PublishResponse response;
  const site_fabric::Status published =
      publisher->publish(declaration, site_fabric::PublishAttempt{1, 1}, response);
  if (!options.quiet) {
    std::printf("MEMBER_PUBLISH domain=%s status=%s\n", options.domain.c_str(),
                site_fabric::to_string(response.status));
    std::fflush(stdout);
  }
  if (!site_fabric::is_ok(published) && response.status != site_fabric::Status::ALREADY_EXISTS) {
    std::fprintf(stderr, "publish refused: %s\n", site_fabric::to_string(published));
    (void)publisher->close();
    return 5;
  }

  if (options.hold_ms > 0) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.hold_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      site_fabric::HeartbeatResponse heartbeat;
      if (!site_fabric::is_ok(publisher->heartbeat(heartbeat))) {
        break;
      }
    }
  }

  if (!options.quiet) {
    std::printf("MEMBER_DONE domain=%s\n", options.domain.c_str());
    std::fflush(stdout);
  }
  (void)publisher->close();
  return 0;
}
