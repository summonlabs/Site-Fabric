// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/crypto.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <random>
#include <thread>

namespace site_fabric::internal {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] constexpr std::uint32_t big_endian(std::uint32_t value) noexcept {
  return ((value & 0x000000FFU) << 24U) | ((value & 0x0000FF00U) << 8U) |
         ((value & 0x00FF0000U) >> 8U) | ((value & 0xFF000000U) >> 24U);
}

[[nodiscard]] std::uint64_t entropy_seed() {
  std::random_device device;
  std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32U) ^
                       static_cast<std::uint64_t>(device());
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  seed ^= static_cast<std::uint64_t>(now);
  seed ^= static_cast<std::uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return seed;
}

[[nodiscard]] std::mt19937_64& local_engine() {
  static thread_local std::mt19937_64 engine(entropy_seed());
  return engine;
}

}  // namespace

Sha256::Sha256() {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
}

void Sha256::compress(const std::uint8_t* block) {
  std::uint32_t schedule[64];
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                      (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U) |
                      (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U) |
                      static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7U) ^
                             rotr(schedule[index - 15], 18U) ^
                             (schedule[index - 15] >> 3U);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17U) ^
                             rotr(schedule[index - 2], 19U) ^
                             (schedule[index - 2] >> 10U);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) {
  if (finished_) {
    return;
  }
  total_bytes_ += data.size();
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t want = 64 - buffered_;
    const std::size_t take = data.size() < want ? data.size() : want;
    std::memcpy(buffer_.data() + buffered_, data.data(), take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (offset + 64 <= data.size()) {
    compress(data.data() + offset);
    offset += 64;
  }
  if (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    std::memcpy(buffer_.data(), data.data() + offset, remaining);
    buffered_ = remaining;
  }
}

void Sha256::update(std::string_view text) {
  update(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Digest Sha256::finish() {
  Digest digest;
  if (finished_) {
    return digest;
  }

  const std::uint64_t bit_length = total_bytes_ * 8ULL;
  const std::uint8_t pad = 0x80;
  update(std::span<const std::uint8_t>(&pad, 1));
  const std::uint8_t zero = 0x00;
  while (buffered_ != 56) {
    update(std::span<const std::uint8_t>(&zero, 1));
  }
  std::uint8_t length_bytes[8];
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] =
        static_cast<std::uint8_t>((bit_length >> (56U - 8U * index)) & 0xFFU);
  }
  update(std::span<const std::uint8_t>(length_bytes, 8));
  finished_ = true;

  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint32_t word = big_endian(state_[index]);
    std::memcpy(digest.bytes.data() + index * 4, &word, 4);
  }
  return digest;
}

Digest digest_bytes(std::span<const std::uint8_t> data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Digest digest_text(std::string_view text) {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

std::string to_hex(std::span<const std::uint8_t> data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::uint8_t byte : data) {
    out.push_back(kDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kDigits[byte & 0x0FU]);
  }
  return out;
}

std::string to_hex(const Digest& digest) {
  return to_hex(std::span<const std::uint8_t>(digest.bytes.data(), digest.bytes.size()));
}

bool from_hex(std::string_view text, std::vector<std::uint8_t>& out) {
  if (text.size() % 2 != 0) {
    return false;
  }
  out.clear();
  out.reserve(text.size() / 2);
  auto value_of = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
      return character - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const int high = value_of(text[index]);
    const int low = value_of(text[index + 1]);
    if (high < 0 || low < 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

std::uint64_t random_u64() {
  return local_engine()();
}

std::string random_hex(std::size_t byte_count) {
  std::vector<std::uint8_t> bytes(byte_count);
  for (std::size_t index = 0; index < byte_count; ++index) {
    bytes[index] = static_cast<std::uint8_t>(local_engine()() & 0xFFU);
  }
  return to_hex(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

std::uint64_t hash_combine(std::uint64_t seed, std::uint64_t value) noexcept {
  return seed ^ (mix64(value) + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U));
}

}  // namespace site_fabric::internal
