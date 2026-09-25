// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

const SiteId kSite = SiteId::unchecked("site-alpha");
constexpr std::int64_t kNow = 1'000'000;
constexpr std::int64_t kTtl = 10'000;

Incarnation inc(std::uint64_t sequence, const std::string& nonce) {
  return Incarnation(sequence, BootNonce(nonce));
}

}  // namespace

SF_TEST(authority, first_controller_is_granted) {
  AuthorityRegistry registry;
  AuthorityToken token;
  SF_CHECK_EQ(Status::GRANTED,
              registry.acquire_site_authority("controller-1", kSite, Epoch(1), inc(1, "boot-a"),
                                              Generation(1), kNow, kTtl, token));
  SF_CHECK(token.structurally_valid());
  SF_CHECK_EQ(Status::OK, registry.validate(token, kNow));
  SF_CHECK_EQ(Epoch(1), registry.current_epoch(kSite));
}

SF_TEST(authority, lower_epoch_is_fenced_forever) {
  AuthorityRegistry registry;
  AuthorityToken first;
  SF_REQUIRE(is_ok(registry.acquire_site_authority("controller-1", kSite, Epoch(3), inc(1, "a"),
                                                   Generation(1), kNow, kTtl, first)));
  AuthorityToken stale;
  SF_CHECK_EQ(Status::FENCED_EPOCH,
              registry.acquire_site_authority("controller-1", kSite, Epoch(2), inc(1, "a"),
                                              Generation(2), kNow + 1, kTtl, stale));
  // The registry's epoch does not move backwards, so the current token is
  // still valid, and a token minted for the older epoch is not.
  SF_CHECK_EQ(Status::OK, registry.validate(first, kNow + 1));
  AuthorityToken older = first;
  older.epoch = Epoch(2);
  SF_CHECK_EQ(Status::FENCED_EPOCH, registry.validate(older, kNow + 1));
}

SF_TEST(authority, incumbent_holds_the_site) {
  AuthorityRegistry registry;
  AuthorityToken first;
  SF_REQUIRE(is_ok(registry.acquire_site_authority("controller-1", kSite, Epoch(1), inc(3, "a"),
                                                   Generation(1), kNow, kTtl, first)));
  AuthorityToken second;
  SF_CHECK_EQ(Status::AUTHORITY_HELD_ELSEWHERE,
              registry.acquire_site_authority("controller-2", kSite, Epoch(1), inc(4, "b"),
                                              Generation(1), kNow + 1, kTtl, second));
  // After the incumbent lapses, a strictly later incarnation may take over.
  const std::int64_t later = kNow + kTtl + 1;
  SF_CHECK_EQ(Status::GRANTED,
              registry.acquire_site_authority("controller-2", kSite, Epoch(1), inc(4, "b"),
                                              Generation(1), later, kTtl, second));
  // The lease the first controller held was superseded, and it is told which.
  SF_CHECK_EQ(Status::AUTHORITY_HELD_ELSEWHERE, registry.validate(first, later));
}

SF_TEST(authority, renew_and_expire) {
  AuthorityRegistry registry;
  AuthorityToken token;
  SF_REQUIRE(is_ok(registry.acquire_site_authority("controller-1", kSite, Epoch(1), inc(1, "a"),
                                                   Generation(1), kNow, kTtl, token)));
  AuthorityToken renewed;
  SF_CHECK_EQ(Status::RENEWED, registry.renew(token, kNow + kTtl - 1, kTtl, renewed));
  SF_CHECK_EQ(Status::OK, registry.validate(renewed, kNow + kTtl + 5));

  AuthorityToken forged = renewed;
  forged.lease_id += 1;
  SF_CHECK_EQ(Status::UNAUTHORIZED, registry.validate(forged, kNow));
  SF_CHECK_EQ(Status::UNAUTHORIZED, registry.renew(forged, kNow, kTtl, renewed));

  AuthorityToken behind = renewed;
  behind.generation = Generation(1);
  AuthorityToken advanced = renewed;
  SF_REQUIRE(is_ok(registry.acquire_site_authority("controller-1", kSite, Epoch(1), inc(1, "a"),
                                                   Generation(5), kNow, kTtl, advanced)));
  SF_CHECK_EQ(Status::FENCED_GENERATION, registry.renew(behind, kNow, kTtl, renewed));

  // A lapsed lease is not silently extended.
  SF_CHECK_EQ(Status::LEASE_EXPIRED,
              registry.renew(advanced, kNow + 10 * kTtl, kTtl, renewed));
  SF_CHECK_EQ(Status::LEASE_EXPIRED, registry.validate(advanced, kNow + 10 * kTtl));
}

SF_TEST(authority, revoked_lease_never_returns) {
  AuthorityRegistry registry;
  AuthorityToken token;
  SF_REQUIRE(is_ok(registry.acquire_site_authority("controller-1", kSite, Epoch(1), inc(1, "a"),
                                                   Generation(1), kNow, kTtl, token)));
  SF_CHECK_EQ(Status::OK, registry.revoke(token, kNow + 1, "operator"));
  SF_CHECK_EQ(Status::LEASE_REVOKED, registry.validate(token, kNow + 2));
  AuthorityToken renewed;
  SF_CHECK_EQ(Status::LEASE_REVOKED, registry.renew(token, kNow + 2, kTtl, renewed));
  SF_CHECK_EQ(Status::LEASE_REVOKED,
              registry.acquire_site_authority("controller-1", kSite, Epoch(1), inc(1, "a"),
                                              Generation(2), kNow + 2, kTtl, renewed));
}

SF_TEST(authority, member_sequences_are_issued_not_guessed) {
  AuthorityRegistry registry;
  const MemberDomainKey domain = MemberDomainKey::rack("rack-a");

  std::uint64_t assigned = 0;
  SF_CHECK_EQ(Status::OK,
              registry.admit_member_incarnation(domain, inc(0, "boot-one"), kNow, &assigned));
  SF_CHECK_EQ(std::uint64_t(1), assigned);
  SF_CHECK_EQ(std::uint64_t(2), registry.next_member_sequence(domain));

  // The same process asking again is idempotent.
  std::uint64_t again = 0;
  SF_CHECK_EQ(Status::ALREADY_EXISTS,
              registry.admit_member_incarnation(domain, inc(1, "boot-one"), kNow, &again));
  SF_CHECK_EQ(std::uint64_t(1), again);

  // A different process claiming the same sequence is refused.
  SF_CHECK_EQ(Status::FENCED_INCARNATION,
              registry.admit_member_incarnation(domain, inc(1, "boot-two"), kNow, nullptr));

  // A fresh process is issued the next sequence.
  std::uint64_t next = 0;
  SF_CHECK_EQ(Status::OK,
              registry.admit_member_incarnation(domain, inc(0, "boot-three"), kNow, &next));
  SF_CHECK_EQ(std::uint64_t(2), next);
  SF_CHECK(registry.member_incarnation_live(domain, inc(2, "boot-three")));
  SF_CHECK(!registry.member_incarnation_live(domain, inc(1, "boot-one")));
}

SF_TEST(authority, dead_authority_never_revives) {
  AuthorityRegistry registry;
  const MemberDomainKey domain = MemberDomainKey::rack("rack-a");
  std::uint64_t assigned = 0;
  SF_REQUIRE(is_ok(registry.admit_member_incarnation(domain, inc(0, "boot-one"), kNow,
                                                     &assigned)));
  SF_REQUIRE(is_ok(registry.retire_member_incarnation(domain, inc(assigned, "boot-one"),
                                                      kNow + 1, "killed")));

  // The exact same process lifetime replaying its incarnation is refused.
  SF_CHECK_EQ(Status::FENCED_INCARNATION,
              registry.admit_member_incarnation(domain, inc(assigned, "boot-one"), kNow + 2,
                                                nullptr));
  // The authority it held is gone.
  SF_CHECK(!registry.member_incarnation_live(domain, inc(assigned, "boot-one")));
  // A genuinely new process is admitted, with a new sequence.
  std::uint64_t next = 0;
  SF_CHECK_EQ(Status::OK,
              registry.admit_member_incarnation(domain, inc(0, "boot-two"), kNow + 3, &next));
  SF_CHECK_EQ(assigned + 1, next);
}

SF_TEST(authority, publication_fences) {
  AuthorityRegistry registry;
  const MemberDomainKey domain = MemberDomainKey::rack("rack-a");
  std::uint64_t assigned = 0;
  SF_REQUIRE(is_ok(registry.admit_member_incarnation(domain, inc(0, "boot-a"), kNow,
                                                     &assigned)));
  const Incarnation live = inc(assigned, "boot-a");
  const Digest first = internal::digest_text("one");

  // Publishing before the registry knows the domain.
  SF_CHECK_EQ(Status::UNAUTHORIZED,
              registry.fence_publication(MemberDomainKey::rack("rack-unknown"), Epoch(1), live,
                                         Generation(1), first, PublishAttempt{1, 1}, kNow));
  // Wrong process lifetime.
  SF_CHECK_EQ(Status::FENCED_INCARNATION,
              registry.fence_publication(domain, Epoch(1), inc(assigned, "boot-z"),
                                         Generation(1), first, PublishAttempt{1, 1}, kNow));
  // A generation of zero is a caller that never set one, not the oldest one.
  SF_CHECK_EQ(Status::INVALID,
              registry.fence_publication(domain, Epoch(1), live, Generation::unset(), first,
                                         PublishAttempt{1, 1}, kNow));
  // A digest of zero carries nothing to compare against.
  SF_CHECK_EQ(Status::INVALID,
              registry.fence_publication(domain, Epoch(1), live, Generation(1), Digest::zero(),
                                         PublishAttempt{1, 1}, kNow));
  SF_CHECK_EQ(Status::OK, registry.fence_publication(domain, Epoch(1), live, Generation(1), first,
                                                     PublishAttempt{1, 1}, kNow));
  SF_CHECK_EQ(Status::OK, registry.record_publication(domain, Epoch(1), Generation(1), first,
                                                      PublishAttempt{1, 1}, kNow));

  // Replaying the accepted attempt is refused.
  SF_CHECK_EQ(Status::FENCED_ATTEMPT,
              registry.fence_publication(domain, Epoch(1), live, Generation(1), first,
                                         PublishAttempt{1, 1}, kNow));
  // The same generation with different content is refused: a generation is
  // immutable.
  SF_CHECK_EQ(Status::FENCED_GENERATION,
              registry.fence_publication(domain, Epoch(1), live, Generation(1),
                                         internal::digest_text("two"), PublishAttempt{2, 1},
                                         kNow));
  // A strictly newer generation with new content is accepted.
  const Digest second = internal::digest_text("two");
  SF_CHECK_EQ(Status::OK, registry.fence_publication(domain, Epoch(1), live, Generation(2),
                                                     second, PublishAttempt{2, 1}, kNow));
  SF_REQUIRE(is_ok(registry.record_publication(domain, Epoch(1), Generation(2), second,
                                               PublishAttempt{2, 1}, kNow)));

  // Now that the high-water mark is generation 2, an older generation is
  // fenced and a lower attempt sequence is a replay.
  SF_CHECK_EQ(Status::FENCED_GENERATION,
              registry.fence_publication(domain, Epoch(1), live, Generation(1), first,
                                         PublishAttempt{3, 1}, kNow));
  SF_CHECK_EQ(Status::REPLAYED,
              registry.fence_publication(domain, Epoch(1), live, Generation(2), second,
                                         PublishAttempt{1, 1}, kNow));

  // An epoch older than the one the registry has already observed is fenced.
  SF_REQUIRE(is_ok(registry.record_publication(domain, Epoch(5), Generation(3),
                                               internal::digest_text("three"),
                                               PublishAttempt{4, 1}, kNow)));
  SF_CHECK_EQ(Status::FENCED_EPOCH,
              registry.fence_publication(domain, Epoch(4), live, Generation(4),
                                         internal::digest_text("four"), PublishAttempt{5, 1},
                                         kNow));
}

SF_TEST(authority, token_evaluation_is_explicit_about_time) {
  AuthorityToken token;
  token.kind = AuthorityKind::SITE_CONTROLLER;
  token.holder = "controller-1";
  token.site = kSite;
  token.epoch = Epoch(1);
  token.incarnation = inc(1, "boot");
  token.generation = Generation(1);
  token.lease_id = 7;
  token.issued_at_ms = kNow;
  token.expires_at_ms = kNow + kTtl;
  SF_CHECK_EQ(Status::OK, token.evaluate_at(kNow));
  SF_CHECK_EQ(Status::LEASE_NOT_YET_VALID, token.evaluate_at(kNow - 1));
  SF_CHECK_EQ(Status::LEASE_EXPIRED, token.evaluate_at(kNow + kTtl + 1));
  token.lease_id = 0;
  SF_CHECK_EQ(Status::INVALID, token.evaluate_at(kNow));
}
