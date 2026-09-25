// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Failure domains.
//
// A failure domain is a set of resources that fail together. Site Fabric
// records two classes, and never lets one masquerade as the other:
//
//   PHYSICAL       - the hardware really does share a fate (a power feed, a
//                    top-of-rack switch, a shared uplink).
//   ADMINISTRATIVE - an operator decided these resources are treated as one
//                    blast radius, whatever the hardware does.
//
// Domains nest. A domain's ancestor chain is part of its identity for the
// purpose of survivability: two resources in the same ancestor share a fate
// even when they sit in different leaf domains.

#ifndef SITE_FABRIC_FAILURE_DOMAIN_HPP
#define SITE_FABRIC_FAILURE_DOMAIN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "site_fabric/evidence.hpp"
#include "site_fabric/identity.hpp"
#include "site_fabric/status.hpp"

namespace site_fabric {

/// Which class of failure domain a claim describes.
enum class FailureDomainClass {
  UNKNOWN = 0,
  PHYSICAL,
  ADMINISTRATIVE,
};

[[nodiscard]] const char* to_string(FailureDomainClass domain_class);
[[nodiscard]] bool parse_failure_domain_class(std::string_view token,
                                              FailureDomainClass& out);
[[nodiscard]] bool is_valid(FailureDomainClass domain_class) noexcept;

/// One member domain's statement about one failure domain.
///
/// parent is optional; a domain with no parent is a root of the forest. The
/// parent may be declared by a different member domain than the child, and the
/// composer accepts that as long as exactly one class and one parent are
/// agreed on.
struct FailureDomainClaim {
  FailureDomainId id;
  FailureDomainClass domain_class = FailureDomainClass::UNKNOWN;
  std::optional<FailureDomainId> parent;
  std::vector<ResourceKey> members;
  Generation generation;
  Digest record_digest;
  Evidence evidence;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const FailureDomainClaim&, const FailureDomainClaim&) = default;
};

/// The composed view of one failure domain.
struct FailureDomainNode {
  FailureDomainId id;
  FailureDomainClass domain_class = FailureDomainClass::UNKNOWN;
  std::optional<FailureDomainId> parent;
  /// Union of every claim's members, canonicalised.
  std::vector<ResourceKey> members;
  /// Direct children, canonicalised.
  std::vector<FailureDomainId> children;
  /// Ancestors, nearest first.
  std::vector<FailureDomainId> ancestors;
  /// Nesting depth; a root has depth zero.
  std::uint32_t depth = 0;
  /// The member domains that asserted this node, canonicalised.
  SourceSet attestors;
  /// OK when the node is agreed, CONFLICTING when the class or parent differ,
  /// FAILURE_DOMAIN_DANGLING_PARENT when the parent is absent from the site.
  Status status = Status::UNKNOWN;

  [[nodiscard]] bool ancestor_of(const FailureDomainId& other) const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// The composed failure-domain forest.
struct FailureDomainForest {
  /// Canonical order: by id.
  std::vector<FailureDomainNode> nodes;
  /// Domains whose ancestry could not be resolved because of a cycle.
  std::vector<FailureDomainId> cyclic;
  /// Domains whose declared parent is not present in the site.
  std::vector<FailureDomainId> dangling;
  Status status = Status::UNKNOWN;
  Factors factors;

  [[nodiscard]] const FailureDomainNode* find(const FailureDomainId& id) const noexcept;

  /// Computes the ancestor chain of every node. Idempotent.
  void resolve_ancestry();

  /// True when the two resources never share a failure domain, including by
  /// ancestry. Unknown domains make the answer false: separateness must be
  /// proven, not assumed.
  [[nodiscard]] bool are_separate(const ResourceKey& left, const ResourceKey& right) const;
};

}  // namespace site_fabric

#endif  // SITE_FABRIC_FAILURE_DOMAIN_HPP
