// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Version and format identifiers.
//
// Three identifiers move independently: the library version, the wire protocol
// version, and the persistence format version. A peer that speaks a different
// wire version is refused rather than reinterpreted, and a store written by a
// different persistence version is refused rather than guessed at.

#ifndef SITE_FABRIC_VERSION_HPP
#define SITE_FABRIC_VERSION_HPP

#include <cstdint>
#include <string>

namespace site_fabric {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Wire protocol version carried by every frame header.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

/// Persistence container version carried by every store file header.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

/// Model schema version carried by every member-domain declaration.
inline constexpr std::uint32_t kModelSchemaMajor = 1;
inline constexpr std::uint32_t kModelSchemaMinor = 0;

[[nodiscard]] std::string version_string();
[[nodiscard]] std::string version_tuple();
[[nodiscard]] std::string build_description();
[[nodiscard]] std::string format_versions();

}  // namespace site_fabric

#endif  // SITE_FABRIC_VERSION_HPP
