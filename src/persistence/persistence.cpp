// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The append-only store.
//
// File layout:
//
//   file header, 48 bytes
//     0..7    magic "SFABSTOR"
//     8..11   format version
//     12..15  reserved, must be zero
//     16..47  SHA-256 of bytes 0..15
//
//   then zero or more records, each
//     header, 80 bytes
//       0..3    magic "SFRC"
//       4..5    record type
//       6..7    flags
//       8..11   payload length
//       12..15  reserved, must be zero
//       16..47  SHA-256 of the payload
//       48..79  SHA-256 of bytes 0..47
//     payload, exactly the declared length
//
// Recovery reads forward and stops at the first record that does not verify.
// What it stops on decides the report: a header that does not fit is a torn
// tail, a header whose digest disagrees is corruption, a payload shorter than
// declared is a torn tail, and a payload whose digest disagrees is corruption.
// The caller sees which. A store whose file header is missing or whose format
// version is not this build's is refused outright.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/digest.hpp"
#include "internal/crypto.hpp"
#include "internal/files.hpp"
#include "site_fabric/persistence.hpp"

namespace site_fabric {
namespace {

constexpr char kFileMagic[8] = {'S', 'F', 'A', 'B', 'S', 'T', 'O', 'R'};
constexpr std::uint32_t kRecordMagic = 0x43524653U;  // 'S','F','R','C' little-endian
constexpr std::size_t kFileHeaderBytes = 48;
constexpr std::size_t kRecordHeaderBytes = 80;

/// Writes four little-endian bytes at an explicit offset. The record header is
/// a fixed-size structure, not an append-only buffer, and writing it with a
/// push-based helper is how a header silently becomes the wrong length.
void store_u32(std::uint8_t* destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>(value & 0xFFU);
  destination[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  destination[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  destination[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

[[nodiscard]] Digest digest_of(std::span<const std::uint8_t> data) {
  return internal::digest_bytes(data);
}

[[nodiscard]] std::vector<std::uint8_t> make_file_header() {
  std::vector<std::uint8_t> header(kFileHeaderBytes, 0);
  std::memcpy(header.data(), kFileMagic, sizeof(kFileMagic));
  const std::uint32_t version_le = kPersistenceFormatVersion;
  header[8] = static_cast<std::uint8_t>(version_le & 0xFFU);
  header[9] = static_cast<std::uint8_t>((version_le >> 8U) & 0xFFU);
  header[10] = static_cast<std::uint8_t>((version_le >> 16U) & 0xFFU);
  header[11] = static_cast<std::uint8_t>((version_le >> 24U) & 0xFFU);
  const Digest digest = digest_of(std::span<const std::uint8_t>(header.data(), 16));
  std::memcpy(header.data() + 16, digest.bytes.data(), digest.bytes.size());
  return header;
}

[[nodiscard]] bool verify_file_header(std::span<const std::uint8_t> data, bool& version_ok,
                                      std::uint32_t& version) {
  if (data.size() < kFileHeaderBytes) {
    return false;
  }
  if (std::memcmp(data.data(), kFileMagic, sizeof(kFileMagic)) != 0) {
    return false;
  }
  const Digest expected = digest_of(data.subspan(0, 16));
  Digest stored;
  std::memcpy(stored.bytes.data(), data.data() + 16, stored.bytes.size());
  if (!(expected == stored)) {
    return false;
  }
  version = get_u32(data.data() + 8);
  version_ok = version == kPersistenceFormatVersion;
  return true;
}

[[nodiscard]] std::vector<std::uint8_t> make_record(RecordType type,
                                                    std::span<const std::uint8_t> payload) {
  if (payload.size() > limits::kMaxRecordPayloadBytes ||
      payload.size() > 0xFFFFFFFFULL) {
    return {};
  }
  std::vector<std::uint8_t> record(kRecordHeaderBytes, 0);
  store_u32(record.data(), kRecordMagic);
  record[4] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(type) & 0xFFU);
  record[5] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(type) >> 8U) & 0xFFU);
  record[6] = 0;
  record[7] = 0;
  store_u32(record.data() + 8, static_cast<std::uint32_t>(payload.size()));
  store_u32(record.data() + 12, 0);
  const Digest payload_digest = digest_of(payload);
  std::memcpy(record.data() + 16, payload_digest.bytes.data(), payload_digest.bytes.size());
  const Digest header_digest = digest_of(std::span<const std::uint8_t>(record.data(), 48));
  std::memcpy(record.data() + 48, header_digest.bytes.data(), header_digest.bytes.size());
  record.insert(record.end(), payload.begin(), payload.end());
  return record;
}

[[nodiscard]] bool publication_precedes(const MemberPublicationRecord& left,
                                        const MemberPublicationRecord& right) {
  if (left.domain != right.domain) {
    return left.domain < right.domain;
  }
  return left.generation < right.generation;
}

}  // namespace

// ---------------------------------------------------------------------------
// Recovery report
// ---------------------------------------------------------------------------

bool StoreRecoveryReport::usable() const noexcept {
  return header_present && !version_incompatible;
}

std::string StoreRecoveryReport::to_string() const {
  std::string out = site_fabric::to_string(status);
  out += " scanned=" + std::to_string(records_scanned);
  out += " accepted=" + std::to_string(records_accepted);
  out += " rejected=" + std::to_string(records_rejected);
  out += " bytes=" + std::to_string(bytes_consumed);
  if (tail_truncated) {
    out += " tail_truncated=" + std::to_string(bytes_truncated);
  }
  if (version_incompatible) {
    out += " version_incompatible";
  }
  if (digest_mismatch) {
    out += " digest_mismatch";
  }
  return out;
}

std::size_t StoreContents::record_count() const {
  return snapshot_history.size() + publications.size() + retirements.size() + tombstones.size() +
         leases.size();
}

Digest StoreContents::content_digest() const {
  internal::CanonicalWriter writer;
  for (const auto& snapshot : snapshot_history) {
    writer.u8(static_cast<std::uint8_t>(RecordType::SNAPSHOT));
    writer.u64(snapshot.sequence);
    writer.digest(snapshot.snapshot_digest);
  }
  for (const auto& record : publications) {
    writer.u8(static_cast<std::uint8_t>(RecordType::PUBLICATION));
    writer.u64(record.acceptance_sequence);
    writer.digest(internal::publication_digest(record));
  }
  for (const auto& record : retirements) {
    writer.u8(static_cast<std::uint8_t>(RecordType::RETIREMENT));
    writer.text(record.domain.to_string());
    writer.generation(record.generation);
  }
  for (const auto& tombstone : tombstones) {
    writer.u8(static_cast<std::uint8_t>(RecordType::TOMBSTONE));
    writer.text(tombstone);
  }
  for (const auto& lease : leases) {
    writer.u8(static_cast<std::uint8_t>(RecordType::LEASE));
    internal::write_authority_token(writer, lease);
  }
  return writer.finish_digest();
}

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

namespace {

/// Decodes one record payload into the content it contributes.
struct DecodedRecord {
  RecordType type = RecordType::UNKNOWN;
  MemberPublicationRecord publication;
  MemberRetirementRecord retirement;
  SiteSnapshot snapshot;
  AuthorityToken lease;
  std::string tombstone;
};

[[nodiscard]] Status decode_record(RecordType type, std::span<const std::uint8_t> payload,
                                   DecodedRecord& out) {
  internal::CanonicalReader reader(payload);
  out.type = type;
  Status status = Status::UNSUPPORTED;
  switch (type) {
    case RecordType::SNAPSHOT:
      status = internal::read_snapshot_body(reader, out.snapshot);
      break;
    case RecordType::PUBLICATION:
      status = internal::read_publication_record(reader, out.publication);
      break;
    case RecordType::RETIREMENT:
      status = internal::read_retirement_record(reader, out.retirement);
      break;
    case RecordType::LEASE:
      status = internal::read_authority_token(reader, out.lease);
      break;
    case RecordType::TOMBSTONE:
      status = reader.text(out.tombstone, limits::kMaxTextBytes);
      break;
    case RecordType::CHECKPOINT:
      status = Status::OK;
      break;
    default:
      return Status::UNSUPPORTED;
  }
  if (!is_ok(status)) {
    return status;
  }
  return reader.finish();
}

}  // namespace

struct SiteStore::Impl {
  mutable std::mutex mutex;
  StoreConfig config;
  std::FILE* handle = nullptr;
  std::uint64_t bytes = 0;
  std::uint32_t records = 0;
  StoreRecoveryReport report;

  /// Commits one encoded record, enforcing both bounds. The caller holds the
  /// store mutex, which is why this is a member and not a free function: it
  /// must never be reachable without the lock.
  [[nodiscard]] Status append_record(RecordType type, std::span<const std::uint8_t> payload,
                                     bool durable) {
    if (handle == nullptr) {
      return Status::STOPPED;
    }
    if (payload.size() > limits::kMaxRecordPayloadBytes) {
      return Status::LIMIT_EXCEEDED;
    }
    const std::vector<std::uint8_t> record = make_record(type, payload);
    if (bytes + record.size() > config.max_bytes) {
      return Status::LIMIT_EXCEEDED;
    }
    if (records + 1 > config.max_records) {
      return Status::LIMIT_EXCEEDED;
    }

    const std::size_t written = std::fwrite(record.data(), 1, record.size(), handle);
    if (written != record.size()) {
      return Status::TRUNCATED;
    }
    if (std::fflush(handle) != 0) {
      return Status::INVALID;
    }
    if (durable) {
      const Status synced = internal::sync_path(config.path);
      if (!is_ok(synced)) {
        return synced;
      }
    }
    bytes += record.size();
    records += 1;
    return Status::OK;
  }

  /// Rewrites the file keeping the newest snapshots and the newest publication
  /// per domain. The caller holds the store mutex.
  [[nodiscard]] Status compact_locked(std::uint64_t& dropped);
};

SiteStore::SiteStore() : impl_(std::make_unique<Impl>()) {}

SiteStore::~SiteStore() {
  if (impl_ != nullptr && impl_->handle != nullptr) {
    std::fclose(impl_->handle);
    impl_->handle = nullptr;
  }
}

bool SiteStore::is_open() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->handle != nullptr;
}

std::uint64_t SiteStore::bytes() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->bytes;
}

std::uint32_t SiteStore::record_count() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->records;
}

const StoreRecoveryReport& SiteStore::recovery() const noexcept { return impl_->report; }

const std::filesystem::path& SiteStore::path() const noexcept { return impl_->config.path; }

namespace {

/// Scans a byte image. Used by both open() and read_file().
[[nodiscard]] Status scan_image(std::span<const std::uint8_t> image, StoreContents& out,
                                std::uint64_t& consumed) {
  consumed = 0;
  out = StoreContents{};
  StoreRecoveryReport& report = out.recovery;

  if (image.size() < kFileHeaderBytes) {
    report.status = Status::NOT_FOUND;
    report.diagnostics.push_back("file smaller than the store header");
    return Status::NOT_FOUND;
  }

  bool version_ok = false;
  std::uint32_t version = 0;
  if (!verify_file_header(image, version_ok, version)) {
    report.status = Status::CORRUPT;
    report.diagnostics.push_back("store header magic or digest mismatch");
    return Status::CORRUPT;
  }
  report.header_present = true;
  report.bytes_consumed = kFileHeaderBytes;
  if (!version_ok) {
    report.version_incompatible = true;
    report.status = Status::UNSUPPORTED_FORMAT;
    report.diagnostics.push_back("store format version " + std::to_string(version) +
                                 " is not " + std::to_string(kPersistenceFormatVersion));
    return Status::UNSUPPORTED_FORMAT;
  }

  std::size_t offset = kFileHeaderBytes;
  Status terminal = Status::OK;
  while (offset < image.size()) {
    const std::size_t remaining = image.size() - offset;
    if (remaining < kRecordHeaderBytes) {
      terminal = Status::TRUNCATED;
      report.tail_truncated = true;
      report.diagnostics.push_back("record header truncated at offset " +
                                   std::to_string(offset));
      break;
    }
    const std::uint8_t* header = image.data() + offset;
    if (get_u32(header) != kRecordMagic) {
      terminal = Status::CORRUPT;
      report.diagnostics.push_back("record magic mismatch at offset " + std::to_string(offset));
      break;
    }
    const Digest header_digest = digest_of(std::span<const std::uint8_t>(header, 48));
    Digest stored_header_digest;
    std::memcpy(stored_header_digest.bytes.data(), header + 48, stored_header_digest.bytes.size());
    if (!(header_digest == stored_header_digest)) {
      terminal = Status::CORRUPT;
      report.digest_mismatch = true;
      report.diagnostics.push_back("record header digest mismatch at offset " +
                                   std::to_string(offset));
      break;
    }

    const std::uint16_t raw_type =
        static_cast<std::uint16_t>(header[4]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(header[5]) << 8U);
    if (raw_type > static_cast<std::uint16_t>(RecordType::CHECKPOINT)) {
      terminal = Status::CORRUPT;
      report.diagnostics.push_back("unknown record type at offset " + std::to_string(offset));
      break;
    }
    const RecordType type = static_cast<RecordType>(raw_type);

    const std::uint32_t payload_bytes = get_u32(header + 8);
    if (payload_bytes > limits::kMaxRecordPayloadBytes) {
      terminal = Status::LIMIT_EXCEEDED;
      report.diagnostics.push_back("record payload above the bound at offset " +
                                   std::to_string(offset));
      break;
    }
    ++report.records_scanned;
    if (remaining - kRecordHeaderBytes < payload_bytes) {
      terminal = Status::TRUNCATED;
      report.tail_truncated = true;
      report.diagnostics.push_back("record payload truncated at offset " +
                                   std::to_string(offset));
      break;
    }

    const std::span<const std::uint8_t> payload =
        image.subspan(offset + kRecordHeaderBytes, payload_bytes);
    Digest stored_payload_digest;
    std::memcpy(stored_payload_digest.bytes.data(), header + 16,
                stored_payload_digest.bytes.size());
    if (!(digest_of(payload) == stored_payload_digest)) {
      terminal = Status::INTEGRITY_FAILURE;
      report.digest_mismatch = true;
      report.diagnostics.push_back("record payload digest mismatch at offset " +
                                   std::to_string(offset));
      break;
    }

    DecodedRecord decoded;
    const Status decode_status = decode_record(type, payload, decoded);
    if (!is_ok(decode_status)) {
      terminal = decode_status;
      report.diagnostics.push_back("record decode failed at offset " +
                                   std::to_string(offset) + ": " +
                                   site_fabric::to_string(decode_status));
      break;
    }

    switch (type) {
      case RecordType::SNAPSHOT: {
        const Digest state_digest = internal::composed_site_digest(decoded.snapshot.state);
        if (!(state_digest == decoded.snapshot.site_digest)) {
          terminal = Status::INTEGRITY_FAILURE;
          report.digest_mismatch = true;
          report.diagnostics.push_back("snapshot state digest mismatch at offset " +
                                       std::to_string(offset));
          break;
        }
        out.snapshot_history.push_back(std::move(decoded.snapshot));
        break;
      }
      case RecordType::PUBLICATION:
        out.publications.push_back(std::move(decoded.publication));
        break;
      case RecordType::RETIREMENT:
        out.retirements.push_back(std::move(decoded.retirement));
        break;
      case RecordType::TOMBSTONE:
        out.tombstones.push_back(std::move(decoded.tombstone));
        break;
      case RecordType::LEASE:
        out.leases.push_back(std::move(decoded.lease));
        break;
      default:
        break;
    }
    if (!is_ok(terminal)) {
      break;
    }

    ++report.records_accepted;
    offset += kRecordHeaderBytes + payload_bytes;
    report.bytes_consumed = offset;
  }

  consumed = offset;
  if (!is_ok(terminal) && terminal != Status::TRUNCATED) {
    ++report.records_rejected;
  }
  if (report.tail_truncated || !is_ok(terminal)) {
    report.bytes_truncated = image.size() - offset;
  }
  report.status = is_ok(terminal) ? Status::OK : terminal;

  std::sort(out.snapshot_history.begin(), out.snapshot_history.end(),
            [](const SiteSnapshot& left, const SiteSnapshot& right) {
              return left.sequence < right.sequence;
            });
  if (!out.snapshot_history.empty()) {
    out.has_snapshot = true;
    out.latest_snapshot = out.snapshot_history.back();
  }
  std::stable_sort(out.publications.begin(), out.publications.end(), publication_precedes);
  return report.status;
}

}  // namespace

Status SiteStore::read_file(const std::filesystem::path& path, StoreContents& out) {
  if (!internal::file_exists(path)) {
    out = StoreContents{};
    out.recovery.status = Status::NOT_FOUND;
    return Status::NOT_FOUND;
  }
  std::vector<std::uint8_t> image;
  const Status read = internal::read_file(path, limits::kMaxMaxStoreBytes, image);
  if (!is_ok(read)) {
    out = StoreContents{};
    out.recovery.status = read;
    return read;
  }
  std::uint64_t consumed = 0;
  return scan_image(std::span<const std::uint8_t>(image.data(), image.size()), out, consumed);
}

Status SiteStore::open(const StoreConfig& config) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->handle != nullptr) {
    std::fclose(impl_->handle);
    impl_->handle = nullptr;
  }
  impl_->config = config;
  impl_->bytes = 0;
  impl_->records = 0;
  impl_->report = StoreRecoveryReport{};

  if (config.path.empty()) {
    return Status::INVALID;
  }
  if (config.max_bytes == 0 || config.max_bytes > limits::kMaxMaxStoreBytes) {
    return Status::INVALID;
  }
  if (config.max_records == 0 || config.max_records > limits::kMaxMaxStoreRecords) {
    return Status::INVALID;
  }

  const Status directory = internal::ensure_directory(config.path.parent_path());
  if (!is_ok(directory)) {
    return directory;
  }

  const bool exists = internal::file_exists(config.path);
  if (!exists) {
    const std::vector<std::uint8_t> header = make_file_header();
    const Status written = internal::write_file_atomic(config.path, header);
    if (!is_ok(written)) {
      return written;
    }
  } else {
    std::vector<std::uint8_t> image;
    const Status read = internal::read_file(config.path, limits::kMaxMaxStoreBytes, image);
    if (!is_ok(read)) {
      impl_->report.status = read;
      return read;
    }
    StoreContents contents;
    std::uint64_t consumed = 0;
    const Status scanned =
        scan_image(std::span<const std::uint8_t>(image.data(), image.size()), contents, consumed);
    impl_->report = contents.recovery;
    if (!impl_->report.usable()) {
      return scanned;
    }
    if (consumed < image.size() && impl_->report.tail_truncated) {
      // A torn tail is cut back so the next append starts from a clean
      // boundary. The report says how much was discarded. Corruption in the
      // middle is not cut away: it is evidence, and a store that failed to
      // decode is refused rather than quietly shortened.
      const Status truncated = internal::truncate_file(config.path, consumed);
      if (!is_ok(truncated)) {
        return truncated;
      }
      impl_->bytes = consumed;
    }
    impl_->records = static_cast<std::uint32_t>(contents.record_count());
  }

  impl_->handle = std::fopen(config.path.string().c_str(), "rb+");
  if (impl_->handle == nullptr) {
    return Status::INVALID;
  }
  if (std::fseek(impl_->handle, 0, SEEK_END) != 0) {
    std::fclose(impl_->handle);
    impl_->handle = nullptr;
    return Status::INVALID;
  }
  const long position = std::ftell(impl_->handle);
  if (position < 0) {
    std::fclose(impl_->handle);
    impl_->handle = nullptr;
    return Status::INVALID;
  }
  impl_->bytes = static_cast<std::uint64_t>(position);

  if (impl_->bytes > config.max_bytes) {
    std::uint64_t dropped = 0;
    const Status compacted = impl_->compact_locked(dropped);
    if (!is_ok(compacted)) {
      return compacted;
    }
  }
  return Status::OK;
}

Status SiteStore::close() {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->handle == nullptr) {
    return Status::OK;
  }
  const int flushed = std::fflush(impl_->handle);
  std::fclose(impl_->handle);
  impl_->handle = nullptr;
  if (flushed != 0) {
    return Status::INVALID;
  }
  return Status::OK;
}


Status SiteStore::append_snapshot(const SiteSnapshot& snapshot) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  internal::CanonicalWriter writer;
  internal::write_snapshot_body(writer, snapshot);
  return impl_->append_record(RecordType::SNAPSHOT, writer.buffer(),
                       impl_->config.durable_commit);
}

Status SiteStore::append_publication(const MemberPublicationRecord& record) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  internal::CanonicalWriter writer;
  internal::write_publication_record(writer, record);
  return impl_->append_record(RecordType::PUBLICATION, writer.buffer(),
                       impl_->config.durable_commit);
}

Status SiteStore::append_retirement(const MemberRetirementRecord& record) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  internal::CanonicalWriter writer;
  internal::write_retirement_record(writer, record);
  return impl_->append_record(RecordType::RETIREMENT, writer.buffer(),
                       impl_->config.durable_commit);
}

Status SiteStore::append_tombstone(const std::string& tombstone) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  internal::CanonicalWriter writer;
  writer.text(tombstone);
  return impl_->append_record(RecordType::TOMBSTONE, writer.buffer(),
                       impl_->config.durable_commit);
}

Status SiteStore::append_lease(const AuthorityToken& token) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  internal::CanonicalWriter writer;
  internal::write_authority_token(writer, token);
  return impl_->append_record(RecordType::LEASE, writer.buffer(), impl_->config.durable_commit);
}

Status SiteStore::load(StoreContents& out) const {
  return read_file(impl_->config.path, out);
}

Status SiteStore::compact(std::uint64_t& dropped) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->compact_locked(dropped);
}

Status SiteStore::Impl::compact_locked(std::uint64_t& dropped) {
  dropped = 0;
  if (config.path.empty()) {
    return Status::INVALID;
  }

  StoreContents contents;
  const Status loaded = read_file(config.path, contents);
  if (!is_ok(loaded) && loaded != Status::TRUNCATED) {
    return loaded;
  }

  std::vector<SiteSnapshot> keep_snapshots;
  const std::size_t limit = config.snapshot_history == 0
                                ? limits::kDefaultSnapshotHistory
                                : config.snapshot_history;
  const std::size_t first = contents.snapshot_history.size() > limit
                                ? contents.snapshot_history.size() - limit
                                : 0;
  for (std::size_t index = first; index < contents.snapshot_history.size(); ++index) {
    keep_snapshots.push_back(contents.snapshot_history[index]);
  }
  dropped += static_cast<std::uint64_t>(first);

  std::map<MemberDomainKey, MemberPublicationRecord> newest_publication;
  for (const auto& record : contents.publications) {
    const auto found = newest_publication.find(record.domain);
    if (found == newest_publication.end() || found->second.generation < record.generation ||
        (found->second.generation == record.generation &&
         found->second.acceptance_sequence < record.acceptance_sequence)) {
      newest_publication[record.domain] = record;
    }
  }
  dropped += static_cast<std::uint64_t>(contents.publications.size() - newest_publication.size());

  std::vector<std::uint8_t> image = make_file_header();
  const auto append_encoded = [&image](RecordType type, const std::vector<std::uint8_t>& payload) {
    const std::vector<std::uint8_t> record = make_record(type, payload);
    image.insert(image.end(), record.begin(), record.end());
  };

  for (const auto& snapshot : keep_snapshots) {
    internal::CanonicalWriter writer;
    internal::write_snapshot_body(writer, snapshot);
    append_encoded(RecordType::SNAPSHOT, writer.buffer());
  }
  for (const auto& [domain, record] : newest_publication) {
    (void)domain;
    internal::CanonicalWriter writer;
    internal::write_publication_record(writer, record);
    append_encoded(RecordType::PUBLICATION, writer.buffer());
  }
  for (const auto& retirement : contents.retirements) {
    internal::CanonicalWriter writer;
    internal::write_retirement_record(writer, retirement);
    append_encoded(RecordType::RETIREMENT, writer.buffer());
  }
  for (const auto& tombstone : contents.tombstones) {
    internal::CanonicalWriter writer;
    writer.text(tombstone);
    append_encoded(RecordType::TOMBSTONE, writer.buffer());
  }
  for (const auto& lease : contents.leases) {
    internal::CanonicalWriter writer;
    internal::write_authority_token(writer, lease);
    append_encoded(RecordType::LEASE, writer.buffer());
  }

  if (image.size() > config.max_bytes) {
    return Status::LIMIT_EXCEEDED;
  }

  if (handle != nullptr) {
    std::fflush(handle);
    std::fclose(handle);
    handle = nullptr;
  }
  const Status written = internal::write_file_atomic(config.path, image);
  if (!is_ok(written)) {
    return written;
  }
  handle = std::fopen(config.path.string().c_str(), "rb+");
  if (handle == nullptr) {
    return Status::INVALID;
  }
  if (std::fseek(handle, 0, SEEK_END) != 0) {
    return Status::INVALID;
  }
  bytes = image.size();
  records = static_cast<std::uint32_t>(keep_snapshots.size() + newest_publication.size() +
                                              contents.retirements.size() +
                                              contents.tombstones.size() + contents.leases.size());
  return Status::OK;
}

}  // namespace site_fabric
