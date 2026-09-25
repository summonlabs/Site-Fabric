// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Hard bounds.
//
// Every bound here is enforced before allocation or arithmetic. A message that
// claims more than a bound allows is refused; it is never partially honoured.

#ifndef SITE_FABRIC_LIMITS_HPP
#define SITE_FABRIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace site_fabric::limits {

// --- Identifiers and strings ----------------------------------------------

inline constexpr std::size_t kMaxIdentifierBytes = 192;
inline constexpr std::size_t kMaxTextBytes = 4096;
inline constexpr std::size_t kMaxFactors = 64;

// --- Declaration shape ----------------------------------------------------

inline constexpr std::size_t kMaxClaimLists = 16384;
inline constexpr std::size_t kMaxResourceMembers = 4096;
inline constexpr std::size_t kMaxCapacityContributions = 16384;
inline constexpr std::size_t kMaxFailureDomainsPerDeclaration = 4096;
inline constexpr std::size_t kMaxMaintenanceZonesPerDeclaration = 1024;
inline constexpr std::size_t kMaxObligationsPerDeclaration = 1024;

// --- Site-wide composition ------------------------------------------------

inline constexpr std::size_t kMaxMemberDomains = 4096;
inline constexpr std::size_t kMaxExpectedMembers = 4096;
inline constexpr std::size_t kMaxDistinctResources = 65536;
inline constexpr std::size_t kMaxDistinctFailureDomains = 8192;
inline constexpr std::size_t kMaxDistinctMaintenanceZones = 4096;
inline constexpr std::size_t kMaxDistinctObligations = 4096;
inline constexpr std::size_t kMaxConflicts = 8192;
inline constexpr std::size_t kMaxDeficits = 8192;
inline constexpr std::size_t kMaxDecisions = 131072;
inline constexpr std::size_t kMaxDecisionSources = 4096;
inline constexpr std::size_t kMaxAttestorsPerResource = 4096;
inline constexpr std::size_t kMaxFailureDomainDepth = 32;

// --- Capacity -------------------------------------------------------------

/// One petabit per second. A claim above this is refused as implausible rather
/// than allowed to overflow an aggregate.
inline constexpr std::uint64_t kMaxCapacityBps = 1'000'000'000'000'000ULL;

/// Counts (links, gateways, domains) are bounded by the same discipline.
inline constexpr std::uint64_t kMaxCountValue = 1'000'000'000ULL;

/// The longest validity a reporter may claim for one observation. Seven days.
/// A longer claim is not treated as a long-lived fact; it is INVALID.
inline constexpr std::uint64_t kMaxEvidenceTtlMs = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

// --- Wire protocol --------------------------------------------------------

inline constexpr std::size_t kMaxFrameBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMinFrameBytes = 8;
inline constexpr std::size_t kMaxMessageNestingInternal = 64U;

// --- Persistence ----------------------------------------------------------

inline constexpr std::uint64_t kDefaultMaxStoreBytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxMaxStoreBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kDefaultMaxStoreRecords = 65536;
inline constexpr std::uint32_t kMaxMaxStoreRecords = 4U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxSnapshotHistory = 4096;
inline constexpr std::uint32_t kDefaultSnapshotHistory = 256;
inline constexpr std::uint64_t kMaxRecordPayloadBytes = 8ULL * 1024ULL * 1024ULL;

// --- Runtime --------------------------------------------------------------

inline constexpr std::size_t kMaxConnections = 512;
inline constexpr std::size_t kMaxWorkerThreads = 64;
inline constexpr std::size_t kMaxQueuedPublications = 4096;
inline constexpr std::uint32_t kMaxMemberSequenceGap = 1'000'000U;

}  // namespace site_fabric::limits

#endif  // SITE_FABRIC_LIMITS_HPP
