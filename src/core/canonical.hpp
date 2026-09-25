// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical binary codec.
//
// Everything Site Fabric hashes or transmits is written through this writer.
// The encoding is little-endian, length-prefixed, and free of padding, so the
// same value produces the same bytes on every platform and in every build
// configuration. That property is what makes a digest a statement about
// content rather than about a compiler.
//
// The reader is hostile-input-safe. Every length is validated against a bound
// before anything is allocated, every read is bounds-checked, and a short
// buffer produces TRUNCATED rather than a partially populated value.

#ifndef SITE_FABRIC_CORE_CANONICAL_HPP
#define SITE_FABRIC_CORE_CANONICAL_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "site_fabric/generation.hpp"
#include "site_fabric/limits.hpp"
#include "site_fabric/status.hpp"

namespace site_fabric::internal {

class CanonicalWriter {
 public:
  CanonicalWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void flag(bool value);

  void raw(std::span<const std::uint8_t> value);
  /// Length-prefixed bytes with no interpretation.
  void bytes(std::span<const std::uint8_t> value);
  /// Length-prefixed UTF-8 text.
  void text(std::string_view value);
  void digest(const Digest& value);
  void generation(const Generation& value);
  void epoch(const Epoch& value);
  void schema(const SchemaVersion& value);
  void incarnation(const Incarnation& value);
  void source_ref(const SourceRef& value);
  /// A bounded collection size.
  void count(std::size_t value);
  void optional(bool present);

  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] Digest finish_digest() const;
  [[nodiscard]] std::string finish_hex() const;

  void clear() { buffer_.clear(); }

 private:
  std::vector<std::uint8_t> buffer_;
};

class CanonicalReader {
 public:
  explicit CanonicalReader(std::span<const std::uint8_t> data) : data_(data) {}

  Status u8(std::uint8_t& out);
  Status u16(std::uint16_t& out);
  Status u32(std::uint32_t& out);
  Status u64(std::uint64_t& out);
  Status i64(std::int64_t& out);
  Status flag(bool& out);

  /// Returns a view into the buffer. No allocation, no copy.
  Status bytes(std::span<const std::uint8_t>& out);
  Status text(std::string& out, std::size_t max_bytes = limits::kMaxTextBytes);
  Status digest(Digest& out);
  Status generation(Generation& out);
  Status epoch(Epoch& out);
  Status schema(SchemaVersion& out);
  Status incarnation(Incarnation& out);
  Status source_ref(SourceRef& out);

  /// Reads a collection size and validates it against a caller-supplied bound.
  Status count(std::size_t& out, std::size_t bound);
  Status optional(bool& present);

  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  /// Returns OK only when the whole buffer was consumed. Trailing bytes mean
  /// the caller's schema and the sender's disagree.
  Status finish() const;

 private:
  Status need(std::size_t bytes) const;

  std::span<const std::uint8_t> data_;
  std::size_t offset_ = 0;
};

/// Case-sensitive, byte-wise ordering used to canonicalise every collection.
template <class T, class Key>
void sort_by(std::vector<T>& items, Key key) {
  std::sort(items.begin(), items.end(), [&key](const T& left, const T& right) {
    return key(left) < key(right);
  });
}

/// Removes exact duplicates from a sorted range.
template <class T>
void dedupe_sorted(std::vector<T>& items) {
  items.erase(std::unique(items.begin(), items.end()), items.end());
}

}  // namespace site_fabric::internal

#endif  // SITE_FABRIC_CORE_CANONICAL_HPP
