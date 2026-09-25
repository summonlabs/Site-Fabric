// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strongly typed identities.
//
// A rack id and a pod id are both strings. They are not interchangeable, and a
// function that accepts one must not silently accept the other. Every identity
// in Site Fabric is therefore a distinct type built on the same validating
// wrapper, and the compiler enforces the distinction that the wire format
// cannot.

#ifndef SITE_FABRIC_IDENTITY_HPP
#define SITE_FABRIC_IDENTITY_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace site_fabric {

/// True when the text is a legal identity: non-empty, bounded, printable
/// ASCII, starting with an alphanumeric, and free of whitespace, control
/// characters and delimiters that the canonical encoder reserves.
[[nodiscard]] bool is_valid_identifier(std::string_view text) noexcept;

/// Tag types. They are never instantiated; they exist only to make each
/// StrongId a distinct type.
struct SiteIdTag {};
struct ControllerIdTag {};
struct MemberIdTag {};
struct ClusterIdTag {};
struct PodIdTag {};
struct RackIdTag {};
struct GatewayIdTag {};
struct LinkIdTag {};
struct FailureDomainIdTag {};
struct MaintenanceZoneIdTag {};
struct ObligationIdTag {};
struct TenantIdTag {};

/// A validating, ordered, strongly typed identity.
///
/// Construction through the explicit constructor validates and throws
/// std::invalid_argument on malformed text; construction through try_make
/// reports the failure instead. The parse path used by decoders always calls
/// try_make, so untrusted input can never produce an invalid identity.
template <class Tag>
class StrongId {
 public:
  StrongId() = default;

  explicit StrongId(std::string value) : value_(std::move(value)) {
    if (!is_valid_identifier(value_)) {
      throw std::invalid_argument("invalid identifier: " + value_);
    }
  }

  /// Builds an identity, returning false instead of throwing when the text is
  /// malformed.
  [[nodiscard]] static bool try_make(std::string_view text, StrongId& out) {
    if (!is_valid_identifier(text)) {
      return false;
    }
    out.value_.assign(text);
    return true;
  }

  [[nodiscard]] static StrongId unchecked(std::string value) {
    StrongId id;
    id.value_ = std::move(value);
    return id;
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] bool valid() const noexcept { return is_valid_identifier(value_); }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  [[nodiscard]] std::string to_string() const { return value_; }

  friend bool operator==(const StrongId&, const StrongId&) = default;
  friend auto operator<=>(const StrongId&, const StrongId&) = default;

 private:
  std::string value_;
};

using SiteId = StrongId<SiteIdTag>;
using ControllerId = StrongId<ControllerIdTag>;
using MemberId = StrongId<MemberIdTag>;
using ClusterId = StrongId<ClusterIdTag>;
using PodId = StrongId<PodIdTag>;
using RackId = StrongId<RackIdTag>;
using GatewayId = StrongId<GatewayIdTag>;
using LinkId = StrongId<LinkIdTag>;
using FailureDomainId = StrongId<FailureDomainIdTag>;
using MaintenanceZoneId = StrongId<MaintenanceZoneIdTag>;
using ObligationId = StrongId<ObligationIdTag>;
using TenantId = StrongId<TenantIdTag>;

/// The kinds of member domain that may contribute to a site.
///
/// A member domain is an autonomous authority for its own subtree. Site Fabric
/// composes their statements; it never rewrites them.
enum class MemberDomainKind {
  UNKNOWN = 0,
  CLUSTER,
  POD,
  RACK,
  SHARED_RESOURCE,
};

[[nodiscard]] const char* to_string(MemberDomainKind kind);
[[nodiscard]] bool parse_member_domain_kind(std::string_view token, MemberDomainKind& out);
[[nodiscard]] bool is_valid(MemberDomainKind kind) noexcept;

/// The identity of a member domain: a kind plus a validated id.
///
/// The kind participates in the identity, so a cluster named "a" and a rack
/// named "a" are different domains.
struct MemberDomainKey {
  MemberDomainKind kind = MemberDomainKind::UNKNOWN;
  std::string id;

  MemberDomainKey() = default;
  MemberDomainKey(MemberDomainKind kind_value, std::string id_value)
      : kind(kind_value), id(std::move(id_value)) {}

  [[nodiscard]] static MemberDomainKey cluster(std::string id) {
    return MemberDomainKey(MemberDomainKind::CLUSTER, std::move(id));
  }
  [[nodiscard]] static MemberDomainKey pod(std::string id) {
    return MemberDomainKey(MemberDomainKind::POD, std::move(id));
  }
  [[nodiscard]] static MemberDomainKey rack(std::string id) {
    return MemberDomainKey(MemberDomainKind::RACK, std::move(id));
  }
  [[nodiscard]] static MemberDomainKey shared_resource(std::string id) {
    return MemberDomainKey(MemberDomainKind::SHARED_RESOURCE, std::move(id));
  }

  [[nodiscard]] bool valid() const noexcept {
    return is_valid(kind) && is_valid_identifier(id);
  }
  [[nodiscard]] bool empty() const noexcept { return id.empty(); }

  /// Stable canonical form: "<kind-token>:<id>".
  [[nodiscard]] std::string to_string() const;

  /// Parses the canonical form. Rejects an unknown kind token rather than
  /// defaulting to UNKNOWN.
  [[nodiscard]] static bool parse(std::string_view text, MemberDomainKey& out);

  friend bool operator==(const MemberDomainKey&, const MemberDomainKey&) = default;
  friend auto operator<=>(const MemberDomainKey&, const MemberDomainKey&) = default;
};

/// The kinds of resource a member domain may own or reference.
///
/// Ownership is exclusive per resource key. When two member domains claim the
/// same key the composition reports an overlap; it never picks a winner
/// silently.
enum class ResourceKind {
  UNKNOWN = 0,
  CLUSTER,
  POD,
  RACK,
  SHARED_LINK,
  GATEWAY,
  CAPACITY_POOL,
  FAILURE_DOMAIN,
  MAINTENANCE_ZONE,
  OBLIGATION,
};

[[nodiscard]] const char* to_string(ResourceKind kind);
[[nodiscard]] bool parse_resource_kind(std::string_view token, ResourceKind& out);
[[nodiscard]] bool is_valid(ResourceKind kind) noexcept;

/// A resource key: a kind plus a validated id.
struct ResourceKey {
  ResourceKind kind = ResourceKind::UNKNOWN;
  std::string id;

  ResourceKey() = default;
  ResourceKey(ResourceKind kind_value, std::string id_value)
      : kind(kind_value), id(std::move(id_value)) {}

  [[nodiscard]] static ResourceKey cluster(std::string id) {
    return ResourceKey(ResourceKind::CLUSTER, std::move(id));
  }
  [[nodiscard]] static ResourceKey pod(std::string id) {
    return ResourceKey(ResourceKind::POD, std::move(id));
  }
  [[nodiscard]] static ResourceKey rack(std::string id) {
    return ResourceKey(ResourceKind::RACK, std::move(id));
  }
  [[nodiscard]] static ResourceKey shared_link(std::string id) {
    return ResourceKey(ResourceKind::SHARED_LINK, std::move(id));
  }
  [[nodiscard]] static ResourceKey gateway(std::string id) {
    return ResourceKey(ResourceKind::GATEWAY, std::move(id));
  }
  [[nodiscard]] static ResourceKey capacity_pool(std::string id) {
    return ResourceKey(ResourceKind::CAPACITY_POOL, std::move(id));
  }
  [[nodiscard]] static ResourceKey failure_domain(std::string id) {
    return ResourceKey(ResourceKind::FAILURE_DOMAIN, std::move(id));
  }
  [[nodiscard]] static ResourceKey maintenance_zone(std::string id) {
    return ResourceKey(ResourceKind::MAINTENANCE_ZONE, std::move(id));
  }
  [[nodiscard]] static ResourceKey obligation(std::string id) {
    return ResourceKey(ResourceKind::OBLIGATION, std::move(id));
  }

  [[nodiscard]] bool valid() const noexcept {
    return is_valid(kind) && is_valid_identifier(id);
  }
  [[nodiscard]] bool empty() const noexcept { return id.empty(); }

  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] static bool parse(std::string_view text, ResourceKey& out);

  friend bool operator==(const ResourceKey&, const ResourceKey&) = default;
  friend auto operator<=>(const ResourceKey&, const ResourceKey&) = default;
};

/// Structural identity of the process that holds authority or publishes.
///
/// A boot nonce is generated once per process lifetime. Two runs of the same
/// binary on the same host therefore have different nonces, which is what
/// makes restart fencing meaningful.
struct BootNonce {
  std::string value;

  BootNonce() = default;
  explicit BootNonce(std::string text) : value(std::move(text)) {}

  [[nodiscard]] bool valid() const noexcept { return is_valid_identifier(value); }
  [[nodiscard]] std::string to_string() const { return value; }

  friend bool operator==(const BootNonce&, const BootNonce&) = default;
  friend auto operator<=>(const BootNonce&, const BootNonce&) = default;
};

}  // namespace site_fabric

namespace std {

template <class Tag>
struct hash<site_fabric::StrongId<Tag>> {
  size_t operator()(const site_fabric::StrongId<Tag>& id) const noexcept {
    return std::hash<std::string>{}(id.value());
  }
};

template <>
struct hash<site_fabric::MemberDomainKey> {
  size_t operator()(const site_fabric::MemberDomainKey& key) const noexcept {
    size_t seed = std::hash<int>{}(static_cast<int>(key.kind));
    seed ^= std::hash<std::string>{}(key.id) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

template <>
struct hash<site_fabric::ResourceKey> {
  size_t operator()(const site_fabric::ResourceKey& key) const noexcept {
    size_t seed = std::hash<int>{}(static_cast<int>(key.kind));
    seed ^= std::hash<std::string>{}(key.id) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

}  // namespace std

#endif  // SITE_FABRIC_IDENTITY_HPP
