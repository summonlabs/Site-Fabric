// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

const SiteId kSite = SiteId::unchecked("site-alpha");
constexpr std::int64_t kNow = 1'000'000;
constexpr std::int64_t kTtl = 100'000;

SyntheticConfig small_config() {
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

struct Fixture {
  ComposedSite composed;
  SiteSnapshot snapshot;
  Epoch epoch;
  Generation generation;
  Incarnation incarnation;
};

/// The incarnation every fixture snapshot is published by. A snapshot names
/// the process that produced it, and the chain fences on exactly that, so the
/// fixture and the token have to agree for the test to mean anything.
const Incarnation kControllerIncarnation(1, BootNonce(std::string("a")));

bool build_fixture(Fixture& out, Epoch epoch = Epoch(1), Generation generation = Generation(1),
                   Incarnation incarnation = kControllerIncarnation, std::int64_t now = kNow) {
  SyntheticConfig config = small_config();
  config.now_ms = kNow;
  SyntheticSite generated;
  if (!is_ok(generate_synthetic_site(config, generated))) {
    return false;
  }
  CompositionInput input = generated.input;
  input.incarnation = incarnation;
  input.epoch = epoch;
  input.generation = generation;
  input.now_ms = now;
  SiteComposer composer;
  if (!is_ok(composer.compose(input, out.composed))) {
    return false;
  }
  out.snapshot = SiteSnapshot{};
  out.snapshot.site = out.composed.site;
  out.epoch = epoch;
  out.generation = generation;
  out.incarnation = incarnation;
  out.snapshot.epoch = out.composed.epoch;
  out.snapshot.generation = out.composed.generation;
  out.snapshot.incarnation = out.composed.incarnation;
  out.snapshot.published_at_ms = now;
  out.snapshot.lifecycle = out.composed.lifecycle;
  out.snapshot.status = out.composed.status;
  out.snapshot.complete = out.composed.lifecycle == SiteLifecycle::CURRENT;
  out.snapshot.state = out.composed;
  return is_ok(out.snapshot.recompute_digest());
}

AuthorityToken token_for(Epoch epoch, std::uint64_t sequence, const std::string& nonce,
                         Generation generation, std::uint64_t lease_id = 1,
                         std::int64_t issued = kNow, std::int64_t expires = kNow + kTtl) {
  AuthorityToken token;
  token.kind = AuthorityKind::SITE_CONTROLLER;
  token.holder = "controller";
  token.site = kSite;
  token.epoch = epoch;
  token.incarnation = Incarnation(sequence, BootNonce(nonce));
  token.generation = generation;
  token.lease_id = lease_id;
  token.issued_at_ms = issued;
  token.expires_at_ms = expires;
  return token;
}

}  // namespace

SF_TEST(snapshots, first_publication_is_numbered_one) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SF_CHECK_EQ(Status::PUBLISHED, chain.publish(fixture.snapshot,
                                               token_for(Epoch(1), 1, "a", Generation(1)), kNow,
                                               result));
  SF_CHECK_EQ(std::uint64_t(1), result.sequence);
  SiteSnapshot current;
  SF_REQUIRE(chain.current(current));
  SF_CHECK_EQ(std::uint64_t(1), current.sequence);
  SF_CHECK(!current.snapshot_digest.is_zero());
  SF_CHECK(current.snapshot_digest == internal::snapshot_envelope_digest(current));
  SF_CHECK(current.site_digest == current.state.compute_digest());
}

SF_TEST(snapshots, identical_republication_does_not_advance) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SF_REQUIRE(is_ok(chain.publish(fixture.snapshot, token_for(Epoch(1), 1, "a", Generation(1)),
                                 kNow, result)));
  SF_CHECK_EQ(Status::ALREADY_EXISTS,
              chain.publish(fixture.snapshot, token_for(Epoch(1), 1, "a", Generation(1)), kNow,
                            result));
  SF_CHECK_EQ(std::size_t(1), chain.size());
  SF_CHECK_EQ(std::uint64_t(1), chain.refusals(Status::ALREADY_EXISTS));
}

SF_TEST(snapshots, a_stale_controller_cannot_publish) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture, Epoch(5), Generation(1),
                           Incarnation(2, BootNonce(std::string("a")))));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SF_REQUIRE(is_ok(chain.publish(fixture.snapshot, token_for(Epoch(5), 2, "a", Generation(1)),
                                 kNow, result)));

  // Lower epoch: the old term never returns.
  SF_CHECK_EQ(Status::FENCED_EPOCH,
              chain.publish(fixture.snapshot, token_for(Epoch(4), 2, "a", Generation(2)), kNow,
                            result));
  // Same epoch, older process lifetime.
  SF_CHECK_EQ(Status::FENCED_INCARNATION,
              chain.publish(fixture.snapshot, token_for(Epoch(5), 1, "b", Generation(2)), kNow,
                            result));
  // Same epoch and incarnation, older generation.
  SF_CHECK_EQ(Status::FENCED_GENERATION,
              chain.publish(fixture.snapshot, token_for(Epoch(5), 2, "a", Generation(0)), kNow,
                            result));
  SF_CHECK_EQ(std::uint64_t(1), chain.refusals(Status::FENCED_EPOCH));
  SF_CHECK_EQ(std::uint64_t(1), chain.refusals(Status::FENCED_INCARNATION));
  SF_CHECK_EQ(std::uint64_t(1), chain.refusals(Status::FENCED_GENERATION));
  SF_CHECK_EQ(std::size_t(1), chain.size());
}

SF_TEST(snapshots, same_generation_different_content_is_fenced) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture, Epoch(1), Generation(4)));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SF_REQUIRE(is_ok(chain.publish(fixture.snapshot, token_for(Epoch(1), 1, "a", Generation(4)),
                                 kNow, result)));

  Fixture other;
  SF_REQUIRE(build_fixture(other, Epoch(1), Generation(4), kControllerIncarnation, kNow + 1));
  SF_CHECK_EQ(Status::FENCED_GENERATION,
              chain.publish(other.snapshot, token_for(Epoch(1), 1, "a", Generation(4)), kNow + 1,
                            result));
}

SF_TEST(snapshots, expired_and_mismatched_tokens_are_refused) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SF_CHECK_EQ(Status::LEASE_EXPIRED,
              chain.publish(fixture.snapshot, token_for(Epoch(1), 1, "a", Generation(1),
                                                        /*lease_id=*/1, kNow, kNow + 5),
                            kNow + 100, result));

  AuthorityToken other_site = token_for(Epoch(1), 1, "a", Generation(1));
  other_site.site = SiteId::unchecked("site-beta");
  SF_CHECK_EQ(Status::SITE_MISMATCH,
              chain.publish(fixture.snapshot, other_site, kNow, result));
  SF_CHECK_EQ(std::size_t(0), chain.size());
}

SF_TEST(snapshots, a_corrupt_snapshot_is_refused) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SiteSnapshot damaged = fixture.snapshot;
  damaged.site_digest = internal::digest_text("not the state");
  SF_CHECK_EQ(Status::INTEGRITY_FAILURE,
              chain.publish(damaged, token_for(Epoch(1), 1, "a", Generation(1)), kNow, result));
  SF_CHECK_EQ(std::size_t(0), chain.size());
}

SF_TEST(snapshots, a_new_epoch_supersedes_the_old_one) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SnapshotChain chain;
  SnapshotPublishResult result;
  SF_REQUIRE(is_ok(chain.publish(fixture.snapshot, token_for(Epoch(1), 1, "a", Generation(1)),
                                 kNow, result)));

  Fixture moved;
  SF_REQUIRE(build_fixture(moved, Epoch(2), Generation(1), kControllerIncarnation, kNow + 1));
  SF_CHECK_EQ(Status::PUBLISHED,
              chain.publish(moved.snapshot, token_for(Epoch(2), 9, "z", Generation(1)), kNow + 1,
                            result));
  SF_CHECK_EQ(std::uint64_t(2), result.sequence);
  SF_CHECK_EQ(Epoch(2), chain.epoch());
  // The previous epoch is now fenced even though its incarnation is newer.
  SF_CHECK_EQ(Status::FENCED_EPOCH,
              chain.publish(moved.snapshot, token_for(Epoch(1), 99, "a", Generation(9)), kNow + 2,
                            result));
}

SF_TEST(snapshots, history_is_bounded_and_queryable) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SnapshotChain chain(3);
  SF_CHECK_EQ(std::size_t(3), chain.history_limit());

  for (int index = 0; index < 6; ++index) {
    Fixture step;
    SF_REQUIRE(build_fixture(step, Epoch(1), Generation(1 + index), kControllerIncarnation,
                             kNow + index));
    SnapshotPublishResult result;
    SF_REQUIRE(is_ok(chain.publish(step.snapshot,
                                   token_for(Epoch(1), 1, "a", Generation(1 + index)),
                                   kNow + index, result)));
  }
  SF_CHECK_EQ(std::size_t(3), chain.size());
  const std::vector<std::uint64_t> sequences = chain.sequences();
  SF_REQUIRE(sequences.size() == 3);
  SF_CHECK_EQ(std::uint64_t(4), sequences.front());
  SF_CHECK_EQ(std::uint64_t(6), sequences.back());
  SF_CHECK(chain.at_sequence(5, fixture.snapshot));
  SF_CHECK(!chain.at_sequence(1, fixture.snapshot));
}

SF_TEST(snapshots, restore_keeps_the_order) {
  Fixture fixture;
  SF_REQUIRE(build_fixture(fixture));
  SiteSnapshot older = fixture.snapshot;
  older.sequence = 4;
  SF_REQUIRE(is_ok(older.recompute_digest()));

  SnapshotChain chain;
  SF_CHECK_EQ(Status::OK, chain.restore(older));
  SF_CHECK_EQ(Status::ALREADY_EXISTS, chain.restore(older));

  Fixture newer;
  SF_REQUIRE(build_fixture(newer, Epoch(1), Generation(1), kControllerIncarnation, kNow + 1));
  newer.snapshot.sequence = 5;
  SF_REQUIRE(is_ok(newer.snapshot.recompute_digest()));
  SF_REQUIRE(is_ok(chain.restore(newer.snapshot)));
  SF_CHECK_EQ(Status::REPLAYED, chain.restore(older));
  SF_CHECK_EQ(std::size_t(2), chain.size());

  // A restored snapshot that does not match its own content is refused.
  SiteSnapshot damaged = newer.snapshot;
  damaged.sequence = 6;
  damaged.site_digest = internal::digest_text("wrong");
  SF_CHECK_EQ(Status::INTEGRITY_FAILURE, chain.restore(damaged));
}
