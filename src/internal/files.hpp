// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// File helpers.
//
// Every write path that must survive a kill writes to a temporary file in the
// same directory, flushes it, and renames it into place. A reader therefore
// never observes a half-written store under the real name. Reads are bounded
// before allocation.

#ifndef SITE_FABRIC_INTERNAL_FILES_HPP
#define SITE_FABRIC_INTERNAL_FILES_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "site_fabric/status.hpp"

namespace site_fabric::internal {

[[nodiscard]] bool file_exists(const std::filesystem::path& path);
[[nodiscard]] bool directory_exists(const std::filesystem::path& path);

/// Reads at most max_bytes. Returns LIMIT_EXCEEDED when the file is larger, so
/// a hostile or corrupt size cannot drive an allocation.
Status read_file(const std::filesystem::path& path, std::uint64_t max_bytes,
                 std::vector<std::uint8_t>& out);

/// Writes atomically: temporary file, flush, rename.
Status write_file_atomic(const std::filesystem::path& path,
                         std::span<const std::uint8_t> data);

/// Appends and flushes. Returns the number of bytes written through written.
Status append_file(const std::filesystem::path& path, std::span<const std::uint8_t> data,
                   std::uint64_t& written);

/// Flushes a file descriptor's contents to the device where the platform
/// supports it.
Status sync_path(const std::filesystem::path& path);

[[nodiscard]] Status file_size(const std::filesystem::path& path, std::uint64_t& out);

Status remove_file(const std::filesystem::path& path);

Status ensure_directory(const std::filesystem::path& path);

Status rename_file(const std::filesystem::path& from, const std::filesystem::path& to);

/// Truncates a file to an exact length. Used to cut a torn tail.
Status truncate_file(const std::filesystem::path& path, std::uint64_t length);

}  // namespace site_fabric::internal

#endif  // SITE_FABRIC_INTERNAL_FILES_HPP
