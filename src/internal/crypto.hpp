// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256, hex and random bytes.
//
// Self-contained: no third-party dependency, no platform crypto API. The
// streaming form exists so that a large stored record can be hashed without
// being materialised twice.

#ifndef SITE_FABRIC_INTERNAL_CRYPTO_HPP
#define SITE_FABRIC_INTERNAL_CRYPTO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "site_fabric/generation.hpp"

namespace site_fabric::internal {

class Sha256 {
 public:
  Sha256();

  void update(std::span<const std::uint8_t> data);
  void update(std::string_view text);
  /// Finalises and returns the digest. The object must not be updated again.
  [[nodiscard]] Digest finish();

 private:
  void compress(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finished_ = false;
};

[[nodiscard]] Digest digest_bytes(std::span<const std::uint8_t> data);
[[nodiscard]] Digest digest_text(std::string_view text);

/// Lower-case hex.
[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> data);
[[nodiscard]] std::string to_hex(const Digest& digest);

/// Strict hex decode. Rejects odd length and any non-hex character.
[[nodiscard]] bool from_hex(std::string_view text, std::vector<std::uint8_t>& out);

/// Cryptographically weak but well-distributed process-local randomness, used
/// for boot nonces and lease ids. This is an identity generator, not a
/// security boundary, and the header says so rather than implying otherwise.
[[nodiscard]] std::uint64_t random_u64();
[[nodiscard]] std::string random_hex(std::size_t byte_count);

/// Mixes a 64-bit value to a well-distributed 64-bit value (splitmix64 final).
[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept;

/// Combines two 64-bit hashes. Used by the canonical encoder's ordering keys.
[[nodiscard]] std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) noexcept;

}  // namespace site_fabric::internal

#endif  // SITE_FABRIC_INTERNAL_CRYPTO_HPP
