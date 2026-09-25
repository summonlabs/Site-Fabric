// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/files.hpp"

#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace site_fabric::internal {
namespace {

Status from_error_code(const std::error_code& code, const std::filesystem::path& path) {
  if (!code) {
    return Status::OK;
  }
  OutcomeBuilder builder(Status::INVALID);
  builder.factor("path", path.string());
  builder.factor("error", code.message());
  return builder.build().status;
}

/// Flushes a C stream to the device. Returns false when the platform refused.
bool sync_stream(std::FILE* stream) {
  if (std::fflush(stream) != 0) {
    return false;
  }
#ifdef _WIN32
  return ::_commit(::_fileno(stream)) == 0;
#else
  return ::fsync(::fileno(stream)) == 0;
#endif
}

}  // namespace

bool file_exists(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::exists(path, code) && !code &&
         std::filesystem::is_regular_file(path, code) && !code;
}

bool directory_exists(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::is_directory(path, code) && !code;
}

Status read_file(const std::filesystem::path& path, std::uint64_t max_bytes,
                 std::vector<std::uint8_t>& out) {
  out.clear();
  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  if (code) {
    return from_error_code(code, path);
  }
  if (size > max_bytes) {
    return Status::LIMIT_EXCEEDED;
  }
  std::FILE* stream = std::fopen(path.string().c_str(), "rb");
  if (stream == nullptr) {
    return Status::NOT_FOUND;
  }
  out.resize(static_cast<std::size_t>(size));
  std::size_t read_total = 0;
  while (read_total < out.size()) {
    const std::size_t got =
        std::fread(out.data() + read_total, 1, out.size() - read_total, stream);
    if (got == 0) {
      break;
    }
    read_total += got;
  }
  const bool short_read = read_total != out.size();
  std::fclose(stream);
  if (short_read) {
    out.clear();
    return Status::TRUNCATED;
  }
  return Status::OK;
}

Status write_file_atomic(const std::filesystem::path& path,
                         std::span<const std::uint8_t> data) {
  std::filesystem::path temporary = path;
  temporary += ".tmp";

  std::FILE* stream = std::fopen(temporary.string().c_str(), "wb");
  if (stream == nullptr) {
    return Status::INVALID;
  }
  std::size_t written = 0;
  while (written < data.size()) {
    const std::size_t put =
        std::fwrite(data.data() + written, 1, data.size() - written, stream);
    if (put == 0) {
      break;
    }
    written += put;
  }
  const bool short_write = written != data.size();
  const bool synced = sync_stream(stream);
  std::fclose(stream);

  if (short_write || !synced) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return short_write ? Status::TRUNCATED : Status::INVALID;
  }

  std::error_code code;
  std::filesystem::rename(temporary, path, code);
  if (code) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return from_error_code(code, path);
  }
  return Status::OK;
}

Status append_file(const std::filesystem::path& path, std::span<const std::uint8_t> data,
                   std::uint64_t& written) {
  written = 0;
  std::FILE* stream = std::fopen(path.string().c_str(), "ab");
  if (stream == nullptr) {
    return Status::INVALID;
  }
  std::size_t total = 0;
  while (total < data.size()) {
    const std::size_t put = std::fwrite(data.data() + total, 1, data.size() - total, stream);
    if (put == 0) {
      break;
    }
    total += put;
  }
  const bool short_write = total != data.size();
  const bool synced = sync_stream(stream);
  std::fclose(stream);
  written = total;
  if (short_write) {
    return Status::TRUNCATED;
  }
  return synced ? Status::OK : Status::INVALID;
}

Status sync_path(const std::filesystem::path& path) {
  std::FILE* stream = std::fopen(path.string().c_str(), "rb+");
  if (stream == nullptr) {
    return Status::NOT_FOUND;
  }
  const bool synced = sync_stream(stream);
  std::fclose(stream);
  return synced ? Status::OK : Status::INVALID;
}

Status file_size(const std::filesystem::path& path, std::uint64_t& out) {
  std::error_code code;
  const auto size = std::filesystem::file_size(path, code);
  if (code) {
    return from_error_code(code, path);
  }
  out = static_cast<std::uint64_t>(size);
  return Status::OK;
}

Status remove_file(const std::filesystem::path& path) {
  std::error_code code;
  const bool removed = std::filesystem::remove(path, code);
  if (code) {
    return from_error_code(code, path);
  }
  return removed ? Status::OK : Status::NOT_FOUND;
}

Status ensure_directory(const std::filesystem::path& path) {
  if (path.empty()) {
    return Status::OK;
  }
  if (directory_exists(path)) {
    return Status::OK;
  }
  std::error_code code;
  std::filesystem::create_directories(path, code);
  if (code) {
    return from_error_code(code, path);
  }
  return directory_exists(path) ? Status::OK : Status::INVALID;
}

Status rename_file(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code code;
  std::filesystem::rename(from, to, code);
  return from_error_code(code, to);
}

Status truncate_file(const std::filesystem::path& path, std::uint64_t length) {
  std::error_code code;
  std::filesystem::resize_file(path, static_cast<std::uintmax_t>(length), code);
  return from_error_code(code, path);
}

}  // namespace site_fabric::internal
