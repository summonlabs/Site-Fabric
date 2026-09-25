// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Generations, epochs, incarnations and digests.
//
// These four types are the whole basis of fencing in Site Fabric. A statement
// is only authoritative if it is attached to the exact generation of the exact
// member domain, observed under the exact site epoch, published by a process
// incarnation that still holds authority. Drop any one of them and the
// statement becomes an opinion.

#ifndef SITE_FABRIC_GENERATION_HPP
#define SITE_FABRIC_GENERATION_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "site_fabric/identity.hpp"

namespace site_fabric {

/// A monotonic per-record version.
///
/// Generation zero means "never set" and is never authoritative. A record
/// moves from generation n to n+1; it never moves backwards, and a statement
/// carrying an older generation is stale by construction.
class Generation {
 public:
  constexpr Generation() = default;
  constexpr explicit Generation(std::uint64_t value) : value_(value) {}

  [[nodiscard]] static constexpr Generation unset() { return Generation(0); }
  [[nodiscard]] static constexpr Generation initial() { return Generation(1); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == 0; }

  /// Next generation. Saturates at the maximum rather than wrapping, and
  /// reports saturation through saturated().
  [[nodiscard]] Generation next() const;

  [[nodiscard]] static constexpr std::uint64_t max_value() {
    return static_cast<std::uint64_t>(0xFFFFFFFFFFFFFFFFULL);
  }
  [[nodiscard]] constexpr bool saturated() const noexcept { return value_ == max_value(); }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const Generation&, const Generation&) = default;
  friend constexpr auto operator<=>(const Generation&, const Generation&) = default;

 private:
  std::uint64_t value_ = 0;
};

/// A site epoch (term).
///
/// Authority in Site Fabric is held per epoch. A controller with a lower epoch
/// than the site has already seen can never publish again, no matter how
/// healthy it looks.
class Epoch {
 public:
  constexpr Epoch() = default;
  constexpr explicit Epoch(std::uint64_t value) : value_(value) {}

  [[nodiscard]] static constexpr Epoch unset() { return Epoch(0); }
  [[nodiscard]] static constexpr Epoch initial() { return Epoch(1); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }

  [[nodiscard]] Epoch next() const;

  [[nodiscard]] static constexpr std::uint64_t max_value() {
    return static_cast<std::uint64_t>(0xFFFFFFFFFFFFFFFFULL);
  }
  [[nodiscard]] constexpr bool saturated() const noexcept { return value_ == max_value(); }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const Epoch&, const Epoch&) = default;
  friend constexpr auto operator<=>(const Epoch&, const Epoch&) = default;

 private:
  std::uint64_t value_ = 0;
};

/// One process lifetime.
///
/// The pair (sequence, boot nonce) identifies a single run of a single
/// process. The sequence orders incarnations of the same logical participant;
/// the nonce distinguishes two processes that were handed the same sequence.
struct Incarnation {
  std::uint64_t sequence = 0;
  BootNonce boot;

  Incarnation() = default;
  Incarnation(std::uint64_t sequence_value, BootNonce boot_nonce)
      : sequence(sequence_value), boot(std::move(boot_nonce)) {}

  [[nodiscard]] bool valid() const noexcept { return sequence != 0 && boot.valid(); }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Incarnation&, const Incarnation&) = default;
  friend auto operator<=>(const Incarnation&, const Incarnation&) = default;
};

/// A 256-bit content digest (SHA-256).
struct Digest {
  std::array<std::uint8_t, 32> bytes{};

  Digest() = default;

  [[nodiscard]] static Digest zero() { return Digest{}; }

  [[nodiscard]] bool is_zero() const noexcept;

  /// Lower-case hex, 64 characters.
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] std::string short_hex() const;

  /// Parses 64 hex characters. Rejects any other length and any non-hex
  /// character; it never truncates or zero-pads.
  [[nodiscard]] static bool parse(std::string_view text, Digest& out);

  friend bool operator==(const Digest&, const Digest&) = default;
  friend auto operator<=>(const Digest&, const Digest&) = default;
};

/// The model schema a declaration was written against.
struct SchemaVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;

  [[nodiscard]] bool compatible_with(const SchemaVersion& supported) const noexcept {
    return major == supported.major && minor <= supported.minor;
  }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const SchemaVersion&, const SchemaVersion&) = default;
  friend auto operator<=>(const SchemaVersion&, const SchemaVersion&) = default;
};

/// An exact provenance edge: this member domain, at this generation, with this
/// digest.
///
/// SourceRef is the only currency in which Site Fabric records why it believes
/// something. Every authoritative field in a composed site resolves to a set
/// of SourceRefs, and a decision whose sources have all changed is invalidated.
struct SourceRef {
  MemberDomainKey domain;
  Generation generation;
  Digest digest;

  SourceRef() = default;
  SourceRef(MemberDomainKey domain_key, Generation generation_value, Digest digest_value)
      : domain(std::move(domain_key)),
        generation(generation_value),
        digest(digest_value) {}

  [[nodiscard]] bool valid() const noexcept {
    return domain.valid() && generation.is_set() && !digest.is_zero();
  }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const SourceRef&, const SourceRef&) = default;
  friend auto operator<=>(const SourceRef&, const SourceRef&) = default;
};

/// Sorted, de-duplicated set of provenance edges.
///
/// A contributing set is always canonical: sorted by (domain, generation,
/// digest) and free of duplicates. Two compositions that saw the same
/// contributions in different orders therefore produce byte-identical sets.
class SourceSet {
 public:
  SourceSet() = default;

  void insert(const SourceRef& ref);
  void insert(const SourceSet& other);

  [[nodiscard]] const std::vector<SourceRef>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] bool contains(const SourceRef& ref) const;
  [[nodiscard]] bool contains_domain(const MemberDomainKey& key) const;
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

  /// True when every edge in this set also appears in other, and the sets are
  /// the same size.
  [[nodiscard]] bool equals(const SourceSet& other) const noexcept {
    return items_ == other.items_;
  }

  /// True when at least one edge in this set is absent from other.
  [[nodiscard]] bool intersects_difference(const SourceSet& other) const;

  [[nodiscard]] std::string to_string() const;

 private:
  std::vector<SourceRef> items_;
  bool truncated_ = false;
  std::size_t dropped_ = 0;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_GENERATION_HPP
