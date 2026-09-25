# Contributing to Site Fabric

We welcome contributions from individuals and organizations.

## Contribution terms

Contributions are submitted under the terms of the Apache License 2.0. No
Contributor License Agreement (CLA) is required. By submitting a contribution
you agree that it may be distributed under the Apache License 2.0.

## Scope and boundaries

Keep changes within the repository's architectural and system boundary. Site
Fabric composes site-level network state from member-domain publications. It
does not own rack, pod or cluster truth, physical discovery, low-level routing,
inter-site control, workload scheduling, power or thermal systems, or
non-network data-center orchestration. Do not expand scope into adjacent
systems, sibling repositories, or unrelated concerns.

## Quality expectations

Changes should meet the repository's normal code-quality, build, test,
documentation, and cleanup expectations. Run the full build and test suite
before opening a pull request, and keep the working tree clean. The project
builds warning-clean with warnings treated as errors; a change that introduces
a first-party warning is not acceptable.

No test carries a timeout. A hang is a defect to diagnose and fix, never
something to terminate or classify as a pass.

## Attribution

Do not add AI attribution or unintended "Co-authored-by" trailers to commit
messages. The commit author is the authoritative attribution.

## Telemetry

Do not introduce telemetry transmission.
