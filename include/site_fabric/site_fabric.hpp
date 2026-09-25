// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Umbrella header.
//
// Including this header pulls in the whole public surface. Every other header
// in the include tree is self-contained, so a consumer that needs only the
// composer can include composition.hpp and nothing else.

#ifndef SITE_FABRIC_SITE_FABRIC_HPP
#define SITE_FABRIC_SITE_FABRIC_HPP

#include "site_fabric/authority.hpp"
#include "site_fabric/capacity.hpp"
#include "site_fabric/composition.hpp"
#include "site_fabric/controller.hpp"
#include "site_fabric/evidence.hpp"
#include "site_fabric/failure_domain.hpp"
#include "site_fabric/generation.hpp"
#include "site_fabric/identity.hpp"
#include "site_fabric/limits.hpp"
#include "site_fabric/maintenance.hpp"
#include "site_fabric/member.hpp"
#include "site_fabric/persistence.hpp"
#include "site_fabric/protocol.hpp"
#include "site_fabric/publisher.hpp"
#include "site_fabric/resource.hpp"
#include "site_fabric/snapshot.hpp"
#include "site_fabric/status.hpp"
#include "site_fabric/synthetic.hpp"
#include "site_fabric/version.hpp"

namespace site_fabric {

/// One-line human summary of a composed site, used by the tools and by the
/// tests when a failure needs context.
[[nodiscard]] std::string summarize(const ComposedSite& site);

/// One-line human summary of a frame header, used by the tools.
[[nodiscard]] std::string summarize(const FrameHeader& header);

/// Renders a site lifecycle together with the factors that produced it.
[[nodiscard]] std::string explain(const ComposedSite& site);

}  // namespace site_fabric

#endif  // SITE_FABRIC_SITE_FABRIC_HPP
