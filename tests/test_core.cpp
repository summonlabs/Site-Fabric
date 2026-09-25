// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <string>
#include <vector>

#include "core/canonical.hpp"
#include "core/digest.hpp"
#include "site_fabric/site_fabric.hpp"
#include "test_support.hpp"

using namespace site_fabric;

SF_TEST(core, identifier_validation) {
  SF_CHECK(is_valid_identifier("rack-1"));
  SF_CHECK(is_valid_identifier("a"));
  SF_CHECK(is_valid_identifier("A.b_c:d-9"));
  SF_CHECK(!is_valid_identifier(""));
  SF_CHECK(!is_valid_identifier("-leading"));
  SF_CHECK(!is_valid_identifier("trailing-"));
  SF_CHECK(!is_valid_identifier("has space"));
  SF_CHECK(!is_valid_identifier("has/slash"));
  SF_CHECK(!is_valid_identifier(std::string(193, 'a')));
  SF_CHECK(is_valid_identifier(std::string(192, 'a')));
}

SF_TEST(core, member_domain_key_round_trip) {
  const MemberDomainKey key = MemberDomainKey::rack("rack-1");
  SF_CHECK_EQ(std::string("RACK:rack-1"), key.to_string());
  MemberDomainKey parsed;
  SF_REQUIRE(MemberDomainKey::parse(key.to_string(), parsed));
  SF_CHECK(parsed == key);
  SF_CHECK(!MemberDomainKey::parse("NOPE:rack-1", parsed));
  SF_CHECK(!MemberDomainKey::parse("RACK", parsed));
  SF_CHECK(!MemberDomainKey::parse("RACK:", parsed));
  // The id may itself contain the separator; only the first one splits.
  SF_REQUIRE(MemberDomainKey::parse("CLUSTER:region:one", parsed));
  SF_CHECK_EQ(std::string("region:one"), parsed.id);
}

SF_TEST(core, resource_key_round_trip) {
  const ResourceKey key = ResourceKey::shared_link("link-3");
  SF_CHECK_EQ(std::string("SHARED_LINK:link-3"), key.to_string());
  ResourceKey parsed;
  SF_REQUIRE(ResourceKey::parse(key.to_string(), parsed));
  SF_CHECK(parsed == key);
  SF_CHECK(!ResourceKey::parse("UNKNOWN:link-3", parsed));
}

SF_TEST(core, generation_and_epoch_advance) {
  SF_CHECK(Generation::unset().is_unset());
  SF_CHECK_EQ(std::uint64_t(2), Generation::initial().next().value());
  const Generation saturated(Generation::max_value());
  SF_CHECK_EQ(Generation::max_value(), saturated.next().value());
  SF_CHECK(saturated.saturated());
  SF_CHECK_EQ(std::uint64_t(2), Epoch::initial().next().value());
  SF_CHECK(Epoch(Epoch::max_value()).next() == Epoch(Epoch::max_value()));
}

SF_TEST(core, digest_round_trip_and_rejection) {
  const Digest digest = internal::digest_text("site-fabric");
  SF_CHECK(!digest.is_zero());
  const std::string hex = digest.to_string();
  SF_CHECK_EQ(std::size_t(64), hex.size());
  Digest parsed;
  SF_REQUIRE(Digest::parse(hex, parsed));
  SF_CHECK(parsed == digest);
  SF_CHECK(!Digest::parse(hex.substr(0, 63), parsed));
  SF_CHECK(!Digest::parse(std::string(64, 'z'), parsed));
  SF_CHECK(Digest::zero().is_zero());
}

SF_TEST(core, source_set_is_sorted_and_unique) {
  SourceSet set;
  const SourceRef a(MemberDomainKey::rack("rack-2"), Generation(1),
                    internal::digest_text("a"));
  const SourceRef b(MemberDomainKey::rack("rack-1"), Generation(1),
                    internal::digest_text("b"));
  set.insert(a);
  set.insert(b);
  set.insert(a);
  SF_CHECK_EQ(std::size_t(2), set.size());
  SF_CHECK(set.items().front() == b);
  SF_CHECK(set.contains(a));
  SF_CHECK(set.contains_domain(MemberDomainKey::rack("rack-1")));
  SourceSet other;
  other.insert(b);
  SF_CHECK(set.intersects_difference(other));
  SF_CHECK(!other.intersects_difference(set));
  SF_CHECK(other.equals(other));
}

SF_TEST(core, status_distinctions_are_preserved) {
  SF_CHECK(is_ok(Status::OK));
  SF_CHECK(is_ok(Status::COMPOSED));
  SF_CHECK(!is_ok(Status::UNKNOWN));
  SF_CHECK(!is_ok(Status::STALE));
  SF_CHECK(!is_ok(Status::CONFLICTING));
  SF_CHECK(!is_ok(Status::INCOMPLETE));
  SF_CHECK(!is_ok(Status::INDETERMINATE));
  SF_CHECK(!is_ok(Status::REFUSED));
  SF_CHECK(!is_ok(Status::CANCELLED));
  SF_CHECK(!is_ok(Status::INVALID));
  SF_CHECK(is_knowledge_gap(Status::UNKNOWN));
  SF_CHECK(is_knowledge_gap(Status::EVIDENCE_MISSING));
  SF_CHECK(!is_knowledge_gap(Status::CAPACITY_OVERFLOW));
  SF_CHECK(is_fencing(Status::FENCED_EPOCH));
  SF_CHECK(is_fencing(Status::FENCED_INCARNATION));
  SF_CHECK(is_fencing(Status::FENCED_GENERATION));
  SF_CHECK(is_fencing(Status::FENCED_ATTEMPT));
  SF_CHECK(!is_fencing(Status::INCOMPLETE));
}

SF_TEST(core, status_tokens_round_trip) {
  const std::vector<Status> codes = {
      Status::UNKNOWN,       Status::OK,          Status::STALE,
      Status::CONFLICTING,   Status::INCOMPLETE,  Status::INDETERMINATE,
      Status::REFUSED,       Status::CANCELLED,   Status::INVALID,
      Status::FENCED_EPOCH,  Status::FENCED_INCARNATION, Status::FENCED_GENERATION,
      Status::FENCED_ATTEMPT, Status::REPLAYED,   Status::TRUNCATED,
      Status::CORRUPT,       Status::CAPACITY_OVERFLOW, Status::CAPACITY_UNDERFLOW,
      Status::UNSUPPORTED_FORMAT, Status::NO_AUTHORITATIVE_SOURCE};
  for (const Status code : codes) {
    const std::string token = to_string(code);
    Status parsed = Status::UNKNOWN;
    SF_CHECK(parse_status_exact(token, parsed));
    SF_CHECK(parsed == code);
    SF_CHECK_EQ(code, parse_status(token));
  }
  Status parsed = Status::OK;
  SF_CHECK(!parse_status_exact("NOT_A_STATUS", parsed));
  SF_CHECK(parse_status("NOT_A_STATUS") == Status::UNKNOWN);
}

SF_TEST(core, factors_are_bounded) {
  Factors factors;
  for (std::size_t index = 0; index < limits::kMaxFactors + 10; ++index) {
    factors.add("f" + std::to_string(index));
  }
  SF_CHECK_EQ(limits::kMaxFactors, factors.size());
  SF_CHECK_EQ(std::size_t(10), factors.dropped());
  SF_CHECK(factors.join(",").find("dropped=10") != std::string::npos);
}

SF_TEST(core, evidence_states_do_not_collapse) {
  const std::int64_t now = 1'000'000;
  SF_CHECK(Evidence::measured(now, 1000).evaluate(now) == EvidenceState::FRESH);
  SF_CHECK(Evidence::measured(now - 5000, 1000).evaluate(now) == EvidenceState::STALE);
  SF_CHECK(Evidence::measured(now + 60'000, 1000).evaluate(now) == EvidenceState::FUTURE);
  SF_CHECK(Evidence::synthetic(now, 1000).evaluate(now) == EvidenceState::SYNTHETIC_ONLY);
  SF_CHECK(Evidence::reconstructed(now, 1000).evaluate(now) ==
           EvidenceState::RECONSTRUCTED_NEEDS_REVALIDATION);
  SF_CHECK(Evidence{}.evaluate(now) == EvidenceState::UNKNOWN);
  SF_CHECK(Evidence::measured(0, 1000).evaluate(now) == EvidenceState::INVALID);
  SF_CHECK(Evidence::measured(now, 0).evaluate(now) == EvidenceState::INVALID);
  SF_CHECK(Evidence::measured(now, limits::kMaxEvidenceTtlMs + 1).evaluate(now) ==
           EvidenceState::INVALID);
  SF_CHECK(is_physical_evidence(Provenance::MEASURED));
  SF_CHECK(!is_physical_evidence(Provenance::SYNTHETIC));
  SF_CHECK(!is_physical_evidence(Provenance::UNKNOWN));
}

SF_TEST(core, capacity_aggregate_distinguishes_empty_from_zero) {
  CapacityAggregate aggregate;
  SF_CHECK(aggregate.status() == Status::CAPACITY_UNKNOWN);
  SF_CHECK(!aggregate.total().ingress.known);

  const Status added = aggregate.add(CapacityVector::uniform(100));
  SF_CHECK(is_ok(added));
  SF_CHECK(aggregate.status() == Status::OK);
  SF_CHECK_EQ(std::uint64_t(100), aggregate.total().ingress.bps);

  // A zero contribution is a real value, unlike an absent one.
  CapacityAggregate zero;
  SF_CHECK(is_ok(zero.add(CapacityVector::uniform(0))));
  SF_CHECK(zero.total().ingress.known);
  SF_CHECK_EQ(std::uint64_t(0), zero.total().ingress.bps);
}

SF_TEST(core, capacity_unknown_is_absorbing) {
  CapacityAggregate aggregate;
  CapacityVector partial;
  partial.ingress = CapacityValue(10);
  partial.egress = CapacityValue::unknown();
  partial.internal = CapacityValue(30);
  SF_CHECK(is_ok(aggregate.add(partial)));
  SF_CHECK(aggregate.total().ingress.known);
  SF_CHECK(!aggregate.total().egress.known);
  SF_CHECK(aggregate.total().internal.known);
  SF_CHECK(aggregate.status() == Status::CAPACITY_UNKNOWN);

  // A later contribution cannot fill the hole.
  SF_CHECK(is_ok(aggregate.add(CapacityVector::uniform(5))));
  SF_CHECK(!aggregate.total().egress.known);
  SF_CHECK(aggregate.status() == Status::CAPACITY_UNKNOWN);
}

SF_TEST(core, capacity_overflow_is_refused_not_wrapped) {
  CapacityAggregate aggregate;
  const std::uint64_t near = limits::kMaxCapacityBps - 1;
  SF_CHECK(is_ok(aggregate.add(CapacityVector::uniform(near))));
  SF_CHECK(aggregate.add(CapacityVector::uniform(1000)) == Status::CAPACITY_OVERFLOW);
  SF_CHECK(aggregate.overflowed());
  SF_CHECK(aggregate.status() == Status::CAPACITY_OVERFLOW);
  SF_CHECK(!aggregate.total().ingress.known);

  CapacityVector accumulator = CapacityVector::uniform(limits::kMaxCapacityBps);
  SF_CHECK(add_capacity(accumulator, CapacityVector::uniform(1)) == Status::CAPACITY_OVERFLOW);
  SF_CHECK_EQ(limits::kMaxCapacityBps, accumulator.ingress.bps);
}

SF_TEST(core, capacity_channel_arithmetic) {
  CapacityVector value;
  value.ingress = CapacityValue(10);
  value.internal = CapacityValue(4);
  SF_CHECK(!capacity_leq(CapacityVector::uniform(11), value));
  SF_CHECK(capacity_leq(CapacityVector::uniform(4), value));

  CapacityVector difference;
  SF_CHECK(is_ok(subtract_capacity(CapacityVector::uniform(10), CapacityVector::uniform(3),
                                   difference)));
  SF_CHECK_EQ(std::uint64_t(7), difference.ingress.bps);
  SF_CHECK(subtract_capacity(CapacityVector::uniform(3), CapacityVector::uniform(10),
                             difference) == Status::CAPACITY_UNDERFLOW);

  CapacityAggregate aggregate;
  SF_CHECK(is_ok(aggregate.add_channel(CapacityChannel::INTERNAL, CapacityValue(9))));
  SF_CHECK_EQ(std::uint64_t(9), aggregate.total().internal.bps);
  SF_CHECK(!aggregate.channel_known(CapacityChannel::INGRESS));
  SF_CHECK(aggregate.channel_known(CapacityChannel::INTERNAL));
}

SF_TEST(core, declaration_seal_is_stable_and_order_insensitive) {
  std::vector<MemberDomainDeclaration> declarations;
  for (int index = 0; index < 4; ++index) {
    declarations.push_back(sftest::simple_rack_declaration(
        "rack-" + std::to_string(index), "r" + std::to_string(index), 1000, 1'000'000, 60'000));
  }
  SF_REQUIRE(is_ok(declarations[0].seal()));
  const Digest first = declarations[0].digest;
  SF_REQUIRE(is_ok(declarations[0].seal()));
  SF_CHECK(first == declarations[0].digest);

  MemberDomainDeclaration shuffled = declarations[0];
  std::reverse(shuffled.racks.begin(), shuffled.racks.end());
  SF_REQUIRE(is_ok(shuffled.seal()));
  SF_CHECK(first == shuffled.digest);

  MemberDomainDeclaration changed = declarations[0];
  changed.racks[0].local_capacity.internal = CapacityValue(2000);
  SF_REQUIRE(is_ok(changed.seal()));
  SF_CHECK(!(first == changed.digest));
}

SF_TEST(core, codec_round_trips_a_declaration) {
  MemberDomainDeclaration declaration = sftest::simple_rack_declaration(
      "rack-0", "r0", 4096, 1'000'000, 60'000);
  SF_REQUIRE(is_ok(declaration.seal()));

  internal::CanonicalWriter writer;
  internal::write_declaration(writer, declaration);
  MemberDomainDeclaration decoded;
  internal::CanonicalReader reader(writer.buffer());
  SF_REQUIRE(is_ok(internal::read_declaration(reader, decoded)));
  SF_REQUIRE(is_ok(reader.finish()));
  SF_CHECK(decoded.compute_digest() == declaration.digest);
  SF_CHECK(decoded.domain == declaration.domain);
  SF_CHECK_EQ(std::size_t(1), decoded.racks.size());
}

SF_TEST(core, codec_refuses_hostile_input) {
  MemberDomainDeclaration declaration = sftest::simple_rack_declaration(
      "rack-0", "r0", 4096, 1'000'000, 60'000);
  SF_REQUIRE(is_ok(declaration.seal()));
  internal::CanonicalWriter writer;
  internal::write_declaration(writer, declaration);
  const std::vector<std::uint8_t> bytes = writer.buffer();

  // Truncation at every length must be reported, never silently decoded.
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    MemberDomainDeclaration decoded;
    internal::CanonicalReader reader(
        std::span<const std::uint8_t>(bytes.data(), length));
    const Status status = internal::read_declaration(reader, decoded);
    SF_CHECK(!is_ok(status));
  }

  // Trailing bytes mean the schema disagrees.
  {
    std::vector<std::uint8_t> extended = bytes;
    extended.push_back(0);
    MemberDomainDeclaration decoded;
    internal::CanonicalReader reader(extended);
    SF_REQUIRE(is_ok(internal::read_declaration(reader, decoded)));
    SF_CHECK(reader.finish() == Status::MALFORMED);
  }

  // An oversized collection count is refused before allocation.
  {
    std::vector<std::uint8_t> mutated = bytes;
    // The rack count sits at the end of the header; find it by rebuilding.
    internal::CanonicalWriter probe;
    internal::write_member_domain_key(probe, declaration.domain);
    probe.text(declaration.site.value());
    probe.generation(declaration.generation);
    probe.schema(declaration.schema);
    probe.epoch(declaration.observed_epoch);
    internal::write_evidence(probe, declaration.evidence);
    const std::size_t offset = probe.size() + 4 + 4 + 4 + 4;
    SF_REQUIRE(offset + 4 <= mutated.size());
    const std::uint32_t huge = 0xFFFFFFF0U;
    for (int index = 0; index < 4; ++index) {
      mutated[offset + static_cast<std::size_t>(index)] =
          static_cast<std::uint8_t>((huge >> (8 * index)) & 0xFFU);
    }
    MemberDomainDeclaration decoded;
    internal::CanonicalReader reader(mutated);
    const Status status = internal::read_declaration(reader, decoded);
    SF_CHECK(status == Status::LIMIT_EXCEEDED || status == Status::TRUNCATED ||
             status == Status::MALFORMED);
    SF_CHECK(!is_ok(status));
  }

  // An unknown enumeration value is malformed, not a default.
  {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[0] = 0x7FU;
    MemberDomainDeclaration decoded;
    internal::CanonicalReader reader(mutated);
    SF_CHECK(internal::read_declaration(reader, decoded) == Status::MALFORMED);
  }
}

SF_TEST(core, version_identifiers_are_present) {
  SF_CHECK(!version_string().empty());
  SF_CHECK(version_tuple().find("wire") != std::string::npos);
  SF_CHECK(format_versions().find("persistence") != std::string::npos);
}
