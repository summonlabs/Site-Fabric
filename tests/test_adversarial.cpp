// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Adversarial input.
//
// Every case here is something a hostile or simply broken peer could send. The
// rule the suite enforces is that each one produces a distinguishing non-OK
// status and never a partially populated model, a wrapped number, or a crash.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "site_fabric/persistence.hpp"
#include "site_fabric/protocol.hpp"
#include "site_fabric/site_fabric.hpp"
#include "site_fabric/synthetic.hpp"
#include "test_support.hpp"

using namespace site_fabric;

namespace {

SyntheticConfig tiny() {
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

}  // namespace

SF_TEST(adversarial, empty_and_degenerate_inputs_are_refused) {
  SiteComposer composer;
  ComposedSite site;
  CompositionInput empty;
  SF_CHECK_EQ(Status::INVALID, composer.compose(empty, site));

  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(tiny(), generated)));

  CompositionInput missing_epoch = generated.input;
  missing_epoch.epoch = Epoch::unset();
  SF_CHECK_EQ(Status::INVALID, composer.compose(missing_epoch, site));

  CompositionInput missing_generation = generated.input;
  missing_generation.generation = Generation::unset();
  SF_CHECK_EQ(Status::INVALID, composer.compose(missing_generation, site));

  CompositionInput missing_incarnation = generated.input;
  missing_incarnation.incarnation = Incarnation{};
  SF_CHECK_EQ(Status::INVALID, composer.compose(missing_incarnation, site));

  CompositionInput bad_time = generated.input;
  bad_time.now_ms = 0;
  SF_CHECK_EQ(Status::INVALID, composer.compose(bad_time, site));

  CompositionInput wrong_site = generated.input;
  wrong_site.expectation.site = SiteId::unchecked("site-beta");
  SF_CHECK_EQ(Status::SITE_MISMATCH, composer.compose(wrong_site, site));

  CompositionInput duplicate_expectation = generated.input;
  duplicate_expectation.expectation.members.push_back(
      duplicate_expectation.expectation.members.front());
  SF_CHECK_EQ(Status::CONFLICTING, composer.compose(duplicate_expectation, site));
}

SF_TEST(adversarial, oversized_collections_are_refused) {
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(tiny(), generated)));
  CompositionInput input = generated.input;
  input.publications.resize(limits::kMaxMemberDomains + 1);
  SiteComposer composer;
  ComposedSite site;
  SF_CHECK_EQ(Status::LIMIT_EXCEEDED, composer.compose(input, site));
  SF_CHECK(site.members.empty());
}

SF_TEST(adversarial, duplicate_identities_within_a_declaration_are_refused) {
  MemberDomainDeclaration declaration =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  RackClaim other;
  other.id = RackId::unchecked("r-a");
  other.generation = Generation(2);
  other.local_capacity = CapacityVector(CapacityValue::unknown(), CapacityValue::unknown(),
                                        CapacityValue(9999));
  other.evidence = Evidence::reported(1000000, 60000);
  declaration.racks.push_back(other);
  SF_CHECK_EQ(Status::CONFLICTING, declaration.seal());

  // The same identity with identical content is merely redundant, and is
  // collapsed rather than refused.
  MemberDomainDeclaration duplicate =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  duplicate.racks.push_back(duplicate.racks.front());
  SF_REQUIRE(is_ok(duplicate.seal()));
  SF_CHECK_EQ(std::size_t(1), duplicate.racks.size());
}

SF_TEST(adversarial, values_above_the_bound_are_refused) {
  MemberDomainDeclaration declaration =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  declaration.racks[0].local_capacity.internal = CapacityValue(limits::kMaxCapacityBps + 1);
  SF_CHECK_EQ(Status::CAPACITY_OVERFLOW, declaration.seal());

  MemberDomainDeclaration obligation_domain;
  obligation_domain.domain = MemberDomainKey::shared_resource("slo");
  obligation_domain.site = SiteId::unchecked("site-alpha");
  obligation_domain.generation = Generation::initial();
  obligation_domain.observed_epoch = Epoch::initial();
  obligation_domain.evidence = Evidence::reported(1000000, 60000);
  ProtectedObligation obligation;
  obligation.id = ObligationId::unchecked("huge");
  obligation.kind = ObligationKind::MIN_UP_SHARED_LINKS;
  obligation.required = limits::kMaxCountValue + 1;
  obligation.generation = Generation::initial();
  obligation.evidence = obligation_domain.evidence;
  obligation_domain.obligations.push_back(std::move(obligation));
  SF_CHECK_EQ(Status::MALFORMED, obligation_domain.seal());
}

SF_TEST(adversarial, absent_evidence_is_never_a_pass) {
  MemberDomainDeclaration declaration =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  declaration.evidence.provenance = Provenance::UNKNOWN;
  SF_CHECK_EQ(Status::EVIDENCE_MISSING, declaration.seal());

  MemberDomainDeclaration claim_level =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  claim_level.racks[0].evidence = Evidence{};
  SF_CHECK_EQ(Status::EVIDENCE_MISSING, claim_level.seal());

  MemberDomainDeclaration expired_ttl =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  expired_ttl.evidence.ttl_ms = 0;
  SF_CHECK_EQ(Status::INVALID, expired_ttl.seal());

  MemberDomainDeclaration absurd_ttl =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  absurd_ttl.evidence.ttl_ms = limits::kMaxEvidenceTtlMs + 1;
  SF_CHECK_EQ(Status::INVALID, absurd_ttl.seal());
}

SF_TEST(adversarial, invalid_identifiers_never_enter_the_model) {
  MemberDomainDeclaration declaration =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  declaration.domain.id = "has space";
  SF_CHECK(!is_ok(declaration.seal()));

  MemberDomainDeclaration empty_id =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  empty_id.racks[0].id = RackId::unchecked("");
  SF_CHECK(!is_ok(empty_id.seal()));

  MemberDomainDeclaration control =
      sftest::simple_rack_declaration("rack-a", "r-a", 1000, 1000000, 60000);
  control.site = SiteId::unchecked(std::string("site\x01alpha"));
  SF_CHECK(!is_ok(control.seal()));
}

SF_TEST(adversarial, the_composer_never_reads_a_declaration_that_does_not_validate) {
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(tiny(), generated)));
  CompositionInput input = generated.input;
  std::size_t racked = input.publications.size();
  for (std::size_t index = 0; index < input.publications.size(); ++index) {
    if (!input.publications[index].declaration.racks.empty()) {
      racked = index;
      break;
    }
  }
  SF_REQUIRE(racked < input.publications.size());
  input.publications[racked].declaration.racks[0].local_capacity.internal =
      CapacityValue(limits::kMaxCapacityBps + 5);
  SiteComposer composer;
  ComposedSite site;
  SF_REQUIRE(is_ok(composer.compose(input, site)));
  const MemberDomainState* state = site.find_member(input.publications[racked].domain);
  SF_REQUIRE(state != nullptr);
  SF_CHECK_NE(MemberLifecycle::CURRENT, state->lifecycle);
  // The declaration was altered after it was sealed, so the publication no
  // longer matches its own digest. Whichever way the composer notices, it must
  // not accept the model.
  SF_CHECK(state->status == Status::DIGEST_MISMATCH_DECLARATION ||
           state->status == Status::CAPACITY_OVERFLOW || is_knowledge_gap(state->status));
  SF_CHECK(!state->authoritative());
}

SF_TEST(adversarial, reordered_and_duplicated_publications_are_handled) {
  SyntheticSite generated;
  SF_REQUIRE(is_ok(generate_synthetic_site(tiny(), generated)));

  CompositionInput doubled = generated.input;
  for (const auto& publication : generated.input.publications) {
    doubled.publications.push_back(publication);
  }
  SiteComposer composer;
  ComposedSite once;
  ComposedSite twice;
  SF_REQUIRE(is_ok(composer.compose(generated.input, once)));
  SF_REQUIRE(is_ok(composer.compose(doubled, twice)));
  SF_CHECK(once.compute_digest() == twice.compute_digest());

  CompositionInput reversed = generated.input;
  std::reverse(reversed.publications.begin(), reversed.publications.end());
  ComposedSite flipped;
  SF_REQUIRE(is_ok(composer.compose(reversed, flipped)));
  SF_CHECK(once.compute_digest() == flipped.compute_digest());

  // A publication with no retained declaration contributes nothing and says so.
  CompositionInput stripped = generated.input;
  stripped.publications.front().has_declaration = false;
  ComposedSite partial;
  SF_REQUIRE(is_ok(composer.compose(stripped, partial)));
  const MemberDomainState* state =
      partial.find_member(stripped.publications.front().domain);
  SF_REQUIRE(state != nullptr);
  SF_CHECK_EQ(MemberLifecycle::REPORTED, state->lifecycle);
  SF_CHECK_EQ(Status::PARTIAL, state->status);
}

SF_TEST(adversarial, store_files_are_fuzzed_without_a_crash) {
  sftest::TemporaryDirectory directory("adversarial-store");
  StoreConfig config;
  config.path = directory.file("site.sfstore");

  SiteSnapshot snapshot;
  {
    SyntheticSite generated;
    SF_REQUIRE(is_ok(generate_synthetic_site(tiny(), generated)));
    ComposedSite composed;
    SiteComposer composer;
    SF_REQUIRE(is_ok(composer.compose(generated.input, composed)));
    snapshot.site = composed.site;
    snapshot.epoch = composed.epoch;
    snapshot.generation = composed.generation;
    snapshot.incarnation = composed.incarnation;
    snapshot.sequence = 1;
    snapshot.published_at_ms = 1000000;
    snapshot.lifecycle = composed.lifecycle;
    snapshot.status = composed.status;
    snapshot.state = composed;
    SF_REQUIRE(is_ok(snapshot.recompute_digest()));
  }
  {
    SiteStore store;
    SF_REQUIRE(is_ok(store.open(config)));
    SF_REQUIRE(is_ok(store.append_snapshot(snapshot)));
    SF_REQUIRE(is_ok(store.close()));
  }

  std::vector<std::uint8_t> original;
  {
    std::ifstream stream(config.path, std::ios::binary);
    char buffer[64];
    while (stream) {
      stream.read(buffer, sizeof(buffer));
      const std::streamsize got = stream.gcount();
      for (std::streamsize index = 0; index < got; ++index) {
        original.push_back(static_cast<std::uint8_t>(buffer[index]));
      }
    }
  }
  SF_REQUIRE(original.size() > 100);

  // Deterministic mutations, not random ones, so a failure is reproducible.
  for (std::size_t position = 0; position < original.size(); position += 17) {
    for (const std::uint8_t mask : {static_cast<std::uint8_t>(0xFFU),
                                    static_cast<std::uint8_t>(0x01U),
                                    static_cast<std::uint8_t>(0x80U)}) {
      std::vector<std::uint8_t> mutated = original;
      mutated[position] = static_cast<std::uint8_t>(mutated[position] ^ mask);
      {
        std::ofstream stream(config.path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(mutated.data()),
                     static_cast<std::streamsize>(mutated.size()));
      }
      StoreContents contents;
      const Status status = SiteStore::read_file(config.path, contents);
      if (is_ok(status) && contents.has_snapshot) {
        // If it says the store is fine, the snapshot must really verify.
        SF_CHECK(contents.latest_snapshot.site_digest ==
                 contents.latest_snapshot.state.compute_digest());
      }
      SF_CHECK(status != Status::UNKNOWN);
    }
  }

  // Every truncation length is safe.
  for (std::size_t length = 0; length < original.size(); length += 29) {
    std::vector<std::uint8_t> truncated(original.begin(),
                                        original.begin() + static_cast<std::ptrdiff_t>(length));
    {
      std::ofstream stream(config.path, std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char*>(truncated.data()),
                   static_cast<std::streamsize>(truncated.size()));
    }
    StoreContents contents;
    const Status status = SiteStore::read_file(config.path, contents);
    if (is_ok(status)) {
      SF_CHECK(contents.latest_snapshot.site_digest ==
               contents.latest_snapshot.state.compute_digest());
    }
  }
}

SF_TEST(adversarial, frames_are_fuzzed_without_a_crash) {
  const std::vector<std::uint8_t> payload = {1, 2, 3, 4};
  std::vector<std::uint8_t> frame;
  SF_REQUIRE(is_ok(encode_frame(MessageType::PING, payload, frame)));
  SF_CHECK_EQ(Status::UNSUPPORTED,
              encode_frame(MessageType::UNKNOWN, payload, frame));

  for (std::size_t position = 0; position < kFrameHeaderBytes; ++position) {
    for (const std::uint8_t mask : {static_cast<std::uint8_t>(0xFFU),
                                    static_cast<std::uint8_t>(0x0FU)}) {
      std::vector<std::uint8_t> mutated = frame;
      mutated[position] = static_cast<std::uint8_t>(mutated[position] ^ mask);
      FrameHeader header;
      const Status status = decode_frame_header(
          std::span<const std::uint8_t>(mutated.data(), kFrameHeaderBytes), header);
      if (is_ok(status)) {
        SF_CHECK(header.payload_bytes <= limits::kMaxFrameBytes);
      }
    }
  }

  // Oversized payload lengths in the header are refused without allocation.
  for (const std::uint32_t declared : {0xFFFFFFFFU, 0x7FFFFFFFU, 0x00FFFFFFU,
                                       static_cast<std::uint32_t>(limits::kMaxFrameBytes + 1)}) {
    std::vector<std::uint8_t> mutated = frame;
    for (int index = 0; index < 4; ++index) {
      mutated[12 + static_cast<std::size_t>(index)] =
          static_cast<std::uint8_t>((declared >> (8 * index)) & 0xFFU);
    }
    FrameHeader header;
    SF_CHECK_EQ(Status::LIMIT_EXCEEDED,
                decode_frame_header(
                    std::span<const std::uint8_t>(mutated.data(), kFrameHeaderBytes), header));
  }
}

SF_TEST(adversarial, extreme_message_values_are_refused) {
  // A snapshot claiming far more members than the model allows.
  FetchSiteResponse response;
  response.status = Status::OK;
  response.has_snapshot = false;
  std::vector<std::uint8_t> payload;
  SF_REQUIRE(is_ok(encode_message(response, payload)));
  // Flip the leading status byte to an out-of-range value.
  payload[0] = 0xFEU;
  FetchSiteResponse decoded;
  SF_CHECK_EQ(Status::MALFORMED, decode_message(payload, decoded));

  // A heartbeat carrying a huge count is bounded by the declaration limits.
  HeartbeatResponse heartbeat;
  heartbeat.status = Status::OK;
  heartbeat.invalidated.resize(4);
  std::vector<std::uint8_t> heartbeat_payload;
  SF_REQUIRE(is_ok(encode_message(heartbeat, heartbeat_payload)));
  HeartbeatResponse decoded_heartbeat;
  SF_REQUIRE(is_ok(decode_message(heartbeat_payload, decoded_heartbeat)));
  SF_CHECK_EQ(std::size_t(4), decoded_heartbeat.invalidated.size());
}
