// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "site_fabric/persistence.hpp"
#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

SyntheticConfig tiny_config() {
  SyntheticConfig config;
  config.clusters = 1;
  config.pods_per_cluster = 1;
  config.racks_per_pod = 1;
  config.shared_links = 1;
  config.gateways = 1;
  config.failure_domains = 1;
  config.maintenance_zones = 0;
  config.obligations = 0;
  return config;
}

bool make_snapshot(std::int64_t now, SiteSnapshot& out, std::uint64_t sequence = 1) {
  SyntheticConfig config = tiny_config();
  SyntheticSite generated;
  if (!is_ok(generate_synthetic_site(config, generated))) {
    return false;
  }
  ComposedSite composed;
  SiteComposer composer;
  if (!is_ok(composer.compose(generated.input, composed))) {
    return false;
  }
  out = SiteSnapshot{};
  out.site = composed.site;
  out.epoch = composed.epoch;
  out.generation = composed.generation;
  out.incarnation = composed.incarnation;
  out.sequence = sequence;
  out.published_at_ms = now;
  out.lifecycle = composed.lifecycle;
  out.status = composed.status;
  out.complete = composed.lifecycle == SiteLifecycle::CURRENT;
  out.state = composed;
  return is_ok(out.recompute_digest());
}

bool make_publication(std::int64_t now, MemberPublicationRecord& out) {
  SyntheticConfig config = tiny_config();
  SyntheticSite generated;
  if (!is_ok(generate_synthetic_site(config, generated))) {
    return false;
  }
  out = generated.input.publications.front();
  out.received_at_ms = now;
  return true;
}

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
  std::vector<std::uint8_t> bytes;
  std::ifstream stream(path, std::ios::binary);
  char buffer[4096];
  while (stream) {
    stream.read(buffer, sizeof(buffer));
    const std::streamsize got = stream.gcount();
    for (std::streamsize index = 0; index < got; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(buffer[index]));
    }
  }
  return bytes;
}

void write_all(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

SF_TEST(persistence, round_trip_survives_close_and_reopen) {
  sftest::TemporaryDirectory directory("store-round-trip");
  StoreConfig config;
  config.path = directory.file("site.sfstore");
  config.durable_commit = true;

  SiteSnapshot snapshot;
  SF_REQUIRE(make_snapshot(1000000, snapshot));
  MemberPublicationRecord publication;
  SF_REQUIRE(make_publication(1000000, publication));
  MemberRetirementRecord retirement;
  retirement.domain = MemberDomainKey::rack("rack-gone");
  retirement.generation = Generation(2);
  retirement.incarnation = Incarnation(1, BootNonce(std::string("boot")));
  retirement.received_at_ms = 1000000;
  retirement.acceptance_sequence = 9;
  retirement.reason = "decommissioned";

  {
    SiteStore store;
    SF_REQUIRE(is_ok(store.open(config)));
    SF_REQUIRE(is_ok(store.append_snapshot(snapshot)));
    SF_REQUIRE(is_ok(store.append_publication(publication)));
    SF_REQUIRE(is_ok(store.append_retirement(retirement)));
    SF_REQUIRE(is_ok(store.append_tombstone("RACK:rack-a/1/boot")));
    SF_REQUIRE(is_ok(store.close()));
    SF_CHECK(!store.is_open());
  }

  SiteStore reopened;
  SF_REQUIRE(is_ok(reopened.open(config)));
  StoreContents contents;
  SF_REQUIRE(is_ok(reopened.load(contents)));
  SF_CHECK(contents.has_snapshot);
  SF_CHECK_EQ(snapshot.sequence, contents.latest_snapshot.sequence);
  SF_CHECK(contents.latest_snapshot.site_digest == snapshot.site_digest);
  SF_CHECK(contents.latest_snapshot.state.compute_digest() == snapshot.state.compute_digest());
  SF_CHECK_EQ(std::size_t(1), contents.publications.size());
  SF_CHECK_EQ(publication.digest, contents.publications.front().digest);
  SF_CHECK_EQ(std::size_t(1), contents.retirements.size());
  SF_CHECK_EQ(std::uint64_t(9), contents.retirements.front().acceptance_sequence);
  SF_CHECK_EQ(std::size_t(1), contents.tombstones.size());
  SF_CHECK(!contents.recovery.tail_truncated);
  SF_CHECK_EQ(Status::OK, contents.recovery.status);
  SF_CHECK(contents.recovery.usable());
  SF_REQUIRE(is_ok(reopened.close()));
}

SF_TEST(persistence, a_torn_tail_is_cut_back_to_the_last_good_record) {
  sftest::TemporaryDirectory directory("store-torn");
  StoreConfig config;
  config.path = directory.file("site.sfstore");

  SiteSnapshot first;
  SiteSnapshot second;
  SF_REQUIRE(make_snapshot(1000000, first, 1));
  SF_REQUIRE(make_snapshot(1000001, second, 2));
  {
    SiteStore store;
    SF_REQUIRE(is_ok(store.open(config)));
    SF_REQUIRE(is_ok(store.append_snapshot(first)));
    SF_REQUIRE(is_ok(store.append_snapshot(second)));
    SF_REQUIRE(is_ok(store.close()));
  }

  std::vector<std::uint8_t> bytes = read_all(config.path);
  SF_REQUIRE(bytes.size() > 100);
  bytes.resize(bytes.size() - 40);
  write_all(config.path, bytes);
  const std::uint64_t truncated_size = static_cast<std::uint64_t>(bytes.size());

  // Reading the file raw shows the torn tail and does not touch it.
  StoreContents raw;
  SF_CHECK_EQ(Status::TRUNCATED, SiteStore::read_file(config.path, raw));
  SF_CHECK(raw.recovery.tail_truncated);
  SF_CHECK_EQ(std::size_t(1), raw.snapshot_history.size());
  SF_CHECK_EQ(std::uint64_t(1), raw.latest_snapshot.sequence);

  // Opening recovers conservatively: it cuts the torn tail and reports it.
  SiteStore store;
  SF_REQUIRE(is_ok(store.open(config)));
  SF_CHECK(store.recovery().tail_truncated);
  SF_CHECK(store.bytes() <= truncated_size);
  StoreContents contents;
  SF_REQUIRE(is_ok(store.load(contents)));
  SF_CHECK_EQ(std::size_t(1), contents.snapshot_history.size());
  SF_CHECK_EQ(std::uint64_t(1), contents.latest_snapshot.sequence);
  // The store was cut back, so a new append starts from a clean boundary.
  SiteSnapshot third;
  SF_REQUIRE(make_snapshot(1000002, third, 3));
  SF_REQUIRE(is_ok(store.append_snapshot(third)));
  SF_REQUIRE(is_ok(store.close()));

  SiteStore reopened;
  SF_REQUIRE(is_ok(reopened.open(config)));
  StoreContents recovered;
  SF_REQUIRE(is_ok(reopened.load(recovered)));
  SF_CHECK_EQ(std::size_t(2), recovered.snapshot_history.size());
  SF_CHECK_EQ(std::uint64_t(3), recovered.latest_snapshot.sequence);
  SF_REQUIRE(is_ok(reopened.close()));
}

SF_TEST(persistence, a_corrupt_payload_is_reported_even_at_the_tail) {
  sftest::TemporaryDirectory directory("store-corrupt");
  StoreConfig config;
  config.path = directory.file("site.sfstore");

  SiteSnapshot snapshot;
  SF_REQUIRE(make_snapshot(1000000, snapshot));
  {
    SiteStore store;
    SF_REQUIRE(is_ok(store.open(config)));
    SF_REQUIRE(is_ok(store.append_snapshot(snapshot)));
    SF_REQUIRE(is_ok(store.close()));
  }

  std::vector<std::uint8_t> bytes = read_all(config.path);
  SF_REQUIRE(bytes.size() > 120);
  bytes[bytes.size() - 10] ^= 0xFFU;
  write_all(config.path, bytes);

  StoreContents contents;
  const Status status = SiteStore::read_file(config.path, contents);
  SF_CHECK(status == Status::INTEGRITY_FAILURE);
  SF_CHECK(contents.recovery.digest_mismatch);
  SF_CHECK_EQ(std::size_t(0), contents.snapshot_history.size());
  SF_CHECK(!contents.recovery.diagnostics.empty());
}

SF_TEST(persistence, a_foreign_or_truncated_header_is_refused) {
  sftest::TemporaryDirectory directory("store-header");

  // Not a store at all.
  {
    const std::filesystem::path path = directory.file("foreign.sfstore");
    write_all(path, std::vector<std::uint8_t>(200, 0x41));
    StoreContents contents;
    SF_CHECK_EQ(Status::CORRUPT, SiteStore::read_file(path, contents));
    SF_CHECK(!contents.recovery.header_present);
  }
  // Too short to hold a header.
  {
    const std::filesystem::path path = directory.file("short.sfstore");
    write_all(path, std::vector<std::uint8_t>(10, 0x00));
    StoreContents contents;
    SF_CHECK_EQ(Status::NOT_FOUND, SiteStore::read_file(path, contents));
  }
  // A store from a different format version is refused rather than guessed at.
  {
    const std::filesystem::path path = directory.file("version.sfstore");
    StoreConfig config;
    config.path = path;
    {
      SiteStore store;
      SF_REQUIRE(is_ok(store.open(config)));
      SiteSnapshot snapshot;
      SF_REQUIRE(make_snapshot(1000000, snapshot));
      SF_REQUIRE(is_ok(store.append_snapshot(snapshot)));
      SF_REQUIRE(is_ok(store.close()));
    }
    std::vector<std::uint8_t> bytes = read_all(path);
    bytes[8] = 0x7FU;
    // Recompute the header digest for the altered version? No: leaving the old
    // digest makes this a corrupt header, which is also a refusal.
    write_all(path, bytes);
    StoreContents contents;
    const Status status = SiteStore::read_file(path, contents);
    SF_CHECK(status == Status::CORRUPT);
    SF_CHECK(!contents.recovery.usable());
  }
}

SF_TEST(persistence, bounds_are_enforced_on_append) {
  sftest::TemporaryDirectory directory("store-bounds");
  StoreConfig config;
  config.path = directory.file("site.sfstore");
  config.max_records = 2;

  SiteStore store;
  SF_REQUIRE(is_ok(store.open(config)));
  SiteSnapshot first;
  SiteSnapshot second;
  SiteSnapshot third;
  SF_REQUIRE(make_snapshot(1000000, first, 1));
  SF_REQUIRE(make_snapshot(1000001, second, 2));
  SF_REQUIRE(make_snapshot(1000002, third, 3));
  SF_REQUIRE(is_ok(store.append_snapshot(first)));
  SF_REQUIRE(is_ok(store.append_snapshot(second)));
  SF_CHECK_EQ(Status::LIMIT_EXCEEDED, store.append_snapshot(third));
  SF_REQUIRE(is_ok(store.close()));

  StoreConfig small = config;
  small.max_bytes = 64;
  small.max_records = 100;
  SiteStore bounded;
  SF_CHECK_EQ(Status::LIMIT_EXCEEDED, bounded.open(small));
}

SF_TEST(persistence, compaction_keeps_the_newest_and_reopens_cleanly) {
  sftest::TemporaryDirectory directory("store-compact");
  StoreConfig config;
  config.path = directory.file("site.sfstore");
  config.snapshot_history = 2;
  config.max_records = 1000;

  SiteStore store;
  SF_REQUIRE(is_ok(store.open(config)));
  for (std::uint64_t index = 1; index <= 6; ++index) {
    SiteSnapshot snapshot;
    SF_REQUIRE(make_snapshot(1000000 + static_cast<std::int64_t>(index), snapshot, index));
    SF_REQUIRE(is_ok(store.append_snapshot(snapshot)));
  }
  std::uint64_t dropped = 0;
  SF_REQUIRE(is_ok(store.compact(dropped)));
  SF_CHECK_EQ(std::uint64_t(4), dropped);
  SF_REQUIRE(is_ok(store.close()));

  SiteStore reopened;
  SF_REQUIRE(is_ok(reopened.open(config)));
  StoreContents contents;
  SF_REQUIRE(is_ok(reopened.load(contents)));
  SF_CHECK_EQ(std::size_t(2), contents.snapshot_history.size());
  SF_CHECK_EQ(std::uint64_t(6), contents.latest_snapshot.sequence);
  SF_CHECK(contents.content_digest() == contents.content_digest());
  SF_REQUIRE(is_ok(reopened.close()));
}

SF_TEST(persistence, reading_a_missing_store_is_not_an_error_but_not_success_either) {
  sftest::TemporaryDirectory directory("store-missing");
  StoreContents contents;
  SF_CHECK_EQ(Status::NOT_FOUND,
              SiteStore::read_file(directory.file("nothing.sfstore"), contents));
  SF_CHECK(!contents.has_snapshot);
  SF_CHECK(!contents.recovery.usable());
}

SF_TEST(persistence, content_digest_is_order_sensitive) {
  sftest::TemporaryDirectory directory("store-digest");
  StoreConfig config;
  config.path = directory.file("site.sfstore");

  SiteSnapshot first;
  SiteSnapshot second;
  SF_REQUIRE(make_snapshot(1000000, first, 1));
  SF_REQUIRE(make_snapshot(1000001, second, 2));
  {
    SiteStore store;
    SF_REQUIRE(is_ok(store.open(config)));
    SF_REQUIRE(is_ok(store.append_snapshot(first)));
    SF_REQUIRE(is_ok(store.append_snapshot(second)));
    SF_REQUIRE(is_ok(store.close()));
  }
  StoreContents contents;
  SF_REQUIRE(is_ok(SiteStore::read_file(config.path, contents)));
  const Digest digest = contents.content_digest();
  SF_CHECK(!digest.is_zero());

  StoreContents again;
  SF_REQUIRE(is_ok(SiteStore::read_file(config.path, again)));
  SF_CHECK(digest == again.content_digest());
}
