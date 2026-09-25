// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/canonical.hpp"

#include <cstring>

#include "internal/crypto.hpp"

namespace site_fabric::internal {
namespace {

void put_u32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
  buffer.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

}  // namespace

void CanonicalWriter::u8(std::uint8_t value) { buffer_.push_back(value); }

void CanonicalWriter::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void CanonicalWriter::u32(std::uint32_t value) { put_u32(buffer_, value); }

void CanonicalWriter::u64(std::uint64_t value) {
  put_u32(buffer_, static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  put_u32(buffer_, static_cast<std::uint32_t>((value >> 32U) & 0xFFFFFFFFULL));
}

void CanonicalWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void CanonicalWriter::flag(bool value) { u8(value ? 1U : 0U); }

void CanonicalWriter::raw(std::span<const std::uint8_t> value) {
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void CanonicalWriter::bytes(std::span<const std::uint8_t> value) {
  u32(static_cast<std::uint32_t>(value.size()));
  raw(value);
}

void CanonicalWriter::text(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void CanonicalWriter::digest(const Digest& value) {
  raw(std::span<const std::uint8_t>(value.bytes.data(), value.bytes.size()));
}

void CanonicalWriter::generation(const Generation& value) { u64(value.value()); }
void CanonicalWriter::epoch(const Epoch& value) { u64(value.value()); }

void CanonicalWriter::schema(const SchemaVersion& value) {
  u32(value.major);
  u32(value.minor);
}

void CanonicalWriter::incarnation(const Incarnation& value) {
  u64(value.sequence);
  text(value.boot.value);
}

void CanonicalWriter::source_ref(const SourceRef& value) {
  u8(static_cast<std::uint8_t>(value.domain.kind));
  text(value.domain.id);
  generation(value.generation);
  digest(value.digest);
}

void CanonicalWriter::count(std::size_t value) {
  u32(static_cast<std::uint32_t>(value));
}

void CanonicalWriter::optional(bool present) { flag(present); }

Digest CanonicalWriter::finish_digest() const {
  return digest_bytes(std::span<const std::uint8_t>(buffer_.data(), buffer_.size()));
}

std::string CanonicalWriter::finish_hex() const {
  return to_hex(std::span<const std::uint8_t>(buffer_.data(), buffer_.size()));
}

Status CanonicalReader::need(std::size_t bytes) const {
  if (remaining() < bytes) {
    return Status::TRUNCATED;
  }
  return Status::OK;
}

Status CanonicalReader::u8(std::uint8_t& out) {
  const Status ready = need(1);
  if (!is_ok(ready)) {
    return ready;
  }
  out = data_[offset_];
  offset_ += 1;
  return Status::OK;
}

Status CanonicalReader::u16(std::uint16_t& out) {
  const Status ready = need(2);
  if (!is_ok(ready)) {
    return ready;
  }
  out = static_cast<std::uint16_t>(data_[offset_]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8U);
  offset_ += 2;
  return Status::OK;
}

Status CanonicalReader::u32(std::uint32_t& out) {
  const Status ready = need(4);
  if (!is_ok(ready)) {
    return ready;
  }
  out = read_u32(data_, offset_);
  offset_ += 4;
  return Status::OK;
}

Status CanonicalReader::u64(std::uint64_t& out) {
  const Status ready = need(8);
  if (!is_ok(ready)) {
    return ready;
  }
  const std::uint64_t low = read_u32(data_, offset_);
  const std::uint64_t high = read_u32(data_, offset_ + 4);
  out = low | (high << 32U);
  offset_ += 8;
  return Status::OK;
}

Status CanonicalReader::i64(std::int64_t& out) {
  std::uint64_t raw_value = 0;
  const Status read = u64(raw_value);
  if (!is_ok(read)) {
    return read;
  }
  out = static_cast<std::int64_t>(raw_value);
  return Status::OK;
}

Status CanonicalReader::flag(bool& out) {
  std::uint8_t value = 0;
  const Status read = u8(value);
  if (!is_ok(read)) {
    return read;
  }
  if (value > 1U) {
    return Status::MALFORMED;
  }
  out = value == 1U;
  return Status::OK;
}

Status CanonicalReader::bytes(std::span<const std::uint8_t>& out) {
  std::uint32_t length = 0;
  const Status read = u32(length);
  if (!is_ok(read)) {
    return read;
  }
  const Status ready = need(length);
  if (!is_ok(ready)) {
    return ready;
  }
  out = data_.subspan(offset_, length);
  offset_ += length;
  return Status::OK;
}

Status CanonicalReader::text(std::string& out, std::size_t max_bytes) {
  std::uint32_t length = 0;
  const Status read = u32(length);
  if (!is_ok(read)) {
    return read;
  }
  if (length > max_bytes) {
    return Status::LIMIT_EXCEEDED;
  }
  const Status ready = need(length);
  if (!is_ok(ready)) {
    return ready;
  }
  out.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
  offset_ += length;
  return Status::OK;
}

Status CanonicalReader::digest(Digest& out) {
  const Status ready = need(out.bytes.size());
  if (!is_ok(ready)) {
    return ready;
  }
  std::memcpy(out.bytes.data(), data_.data() + offset_, out.bytes.size());
  offset_ += out.bytes.size();
  return Status::OK;
}

Status CanonicalReader::generation(Generation& out) {
  std::uint64_t value = 0;
  const Status read = u64(value);
  if (!is_ok(read)) {
    return read;
  }
  out = Generation(value);
  return Status::OK;
}

Status CanonicalReader::epoch(Epoch& out) {
  std::uint64_t value = 0;
  const Status read = u64(value);
  if (!is_ok(read)) {
    return read;
  }
  out = Epoch(value);
  return Status::OK;
}

Status CanonicalReader::schema(SchemaVersion& out) {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  const Status first = u32(major);
  if (!is_ok(first)) {
    return first;
  }
  const Status second = u32(minor);
  if (!is_ok(second)) {
    return second;
  }
  out.major = major;
  out.minor = minor;
  return Status::OK;
}

Status CanonicalReader::incarnation(Incarnation& out) {
  std::uint64_t sequence = 0;
  const Status first = u64(sequence);
  if (!is_ok(first)) {
    return first;
  }
  std::string boot;
  const Status second = text(boot, limits::kMaxIdentifierBytes);
  if (!is_ok(second)) {
    return second;
  }
  out.sequence = sequence;
  out.boot = BootNonce(std::move(boot));
  return Status::OK;
}

Status CanonicalReader::source_ref(SourceRef& out) {
  std::uint8_t kind = 0;
  const Status first = u8(kind);
  if (!is_ok(first)) {
    return first;
  }
  if (kind > static_cast<std::uint8_t>(MemberDomainKind::SHARED_RESOURCE)) {
    return Status::MALFORMED;
  }
  std::string id;
  const Status second = text(id, limits::kMaxIdentifierBytes);
  if (!is_ok(second)) {
    return second;
  }
  // A member that never reported has no source edge. That absence is part of
  // the composed state and has to survive a round trip intact. The remaining
  // fields are still consumed: a reader that stopped early would leave the
  // stream misaligned for everything after it.
  const bool absent = kind == 0;
  if (absent) {
    if (!id.empty()) {
      return Status::MALFORMED;
    }
  } else if (!is_valid_identifier(id)) {
    return Status::MALFORMED;
  }
  Generation generation_value;
  const Status third = generation(generation_value);
  if (!is_ok(third)) {
    return third;
  }
  Digest digest_value;
  const Status fourth = digest(digest_value);
  if (!is_ok(fourth)) {
    return fourth;
  }
  if (absent) {
    out = SourceRef{};
    return Status::OK;
  }
  out.domain = MemberDomainKey(static_cast<MemberDomainKind>(kind), std::move(id));
  out.generation = generation_value;
  out.digest = digest_value;
  return Status::OK;
}

Status CanonicalReader::count(std::size_t& out, std::size_t bound) {
  std::uint32_t value = 0;
  const Status read = u32(value);
  if (!is_ok(read)) {
    return read;
  }
  if (value > bound) {
    return Status::LIMIT_EXCEEDED;
  }
  out = value;
  return Status::OK;
}

Status CanonicalReader::optional(bool& present) { return flag(present); }

Status CanonicalReader::finish() const {
  return at_end() ? Status::OK : Status::MALFORMED;
}

}  // namespace site_fabric::internal
