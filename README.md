# Site Fabric

Site Fabric is a vendor-neutral runtime that holds the **aggregate network model
and the authority for one physical site**. It composes the authoritative
statements of the site's member domains — clusters, pods, racks and shared
resources — into a single generation-bound site domain, and it decides who is
allowed to change that domain and when.

It answers: *given everything the member domains have authoritatively said, what
network state exists at this site now, which connectivity and capacity remain
authoritative, and what site-level constraints must downstream systems obey?*

It is deliberately not a discovery system, not a router, not a scheduler, and
not an inter-site controller. It consumes member-domain truth; it never invents
it.

* Language: C++20
* Namespace: `site_fabric`
* License: Apache License 2.0
* Version: 1.0.0 (wire protocol 1, persistence format 1, model schema 1.0)
* Dependencies: none beyond the C++ standard library and Winsock
* Telemetry: none

## Contents

* [What it owns](#what-it-owns)
* [What it must never absorb](#what-it-must-never-absorb)
* [Doctrine](#doctrine)
* [Architecture](#architecture)
* [The site model](#the-site-model)
* [Composition](#composition)
* [Capacity](#capacity)
* [Failure domains](#failure-domains)
* [Maintenance and protected obligations](#maintenance-and-protected-obligations)
* [Authority and fencing](#authority-and-fencing)
* [Snapshots](#snapshots)
* [Persistence and recovery](#persistence-and-recovery)
* [Wire protocol and transport](#wire-protocol-and-transport)
* [Concurrency and ownership audit](#concurrency-and-ownership-audit)
* [Building](#building)
* [Installing and consuming](#installing-and-consuming)
* [Tools](#tools)
* [Library API sketch](#library-api-sketch)
* [Tests](#tests)
* [Benchmarks](#benchmarks)
* [Limits](#limits)
* [Project layout](#project-layout)
* [Limitations](#limitations)
* [License](#license)

## What it owns

Site Fabric is the single authority for:

* **Site identity and epoch** — the site id, its generation, the epoch that
  fences every mutation, and the process incarnation that currently holds it.
* **Membership** — which member domains exist, what they claim, and whether
  each one is current. A member domain is a cluster, a pod, a rack or a shared
  resource, identified by kind and id, and it publishes its own subtree.
* **Resource ownership** — who owns each rack, pod, cluster, shared link,
  gateway, capacity pool, failure domain and maintenance zone, with the exact
  member generation and digest that asserted it.
* **Aggregate capacity** — total and available ingress, egress and internal
  capacity, counted once per owned resource, with a checked closing identity.
* **Shared-resource accounting** — a link or gateway declared by several member
  domains is counted once, with every attestor recorded, or reported as an
  accounting conflict when the attestors disagree.
* **Failure domains** — physical and administrative domains, their nesting,
  their members, and their ancestry, with cycles and dangling parents reported
  rather than repaired.
* **Maintenance state** — which zones remove capacity from availability and
  which are refused because they would breach a protected obligation.
* **Protected obligations** — declared floors on capacity, link count, gateway
  count, failure-domain diversity and survivability, with a verdict for each.
* **Connectivity** — per-link and per-gateway state, aggregated, with unknown
  state kept unknown.
* **Mutation authority** — epochs, incarnations, generation CAS, attempt
  fencing and durable death records.
* **Snapshots** — immutable, digest-verified, monotonically numbered
  publications of a composed site.
* **Persistence and conservative recovery** — a versioned, integrity-checked,
  bounded store whose torn tails are cut back and whose corruption is reported.
* **Deterministic explanation** — every field of a composed site resolves to the
  exact member-domain sources it came from, and every rejection carries a stable
  status code and factors.

## What it must never absorb

Site Fabric does not implement, and will not grow into, any of the following.
They are separate systems that consume Site Fabric's description:

* rack, pod or cluster truth itself (Rack Fabric, Pod Fabric, Cluster Fabric)
* physical discovery, LLDP/CDP walking, cable attestation, transceiver
  identification
* low-level routing, route computation, FIB programming, segment routing, path
  selection
* inter-site control, federation, inter-site capacity exchange
* workload scheduling, admission, placement, preemption
* power and thermal systems, cooling control
* non-network data-centre orchestration

Site Fabric may *record* evidence about those things. It never *drives* them.

## Doctrine

These rules are enforced by the implementation and asserted by the test suite.

1. **UNKNOWN is a first-class value.** It never silently becomes CURRENT, UP,
   KNOWN or SATISFIED.
2. **Absence of evidence is not positive evidence.** A missing publication
   produces a deficit; a missing capacity channel stays unreported and does not
   become zero.
3. **Structural validity before interpretation.** Bounds, encodings and integrity
   are checked before any semantic reading.
4. **Favourable aggregates never rescue stale or invalid state.** A site is not
   CURRENT because most members are fine.
5. **Provenance is explicit.** MEASURED, REPORTED, DERIVED, ESTIMATED,
   SYNTHETIC, RECONSTRUCTED, UNKNOWN. Synthetic evidence can never make a
   physical site CURRENT.
6. **Recovered evidence is history, not measurement.** A declaration read back
   from disk keeps its digest and stops being authoritative until its domain
   reports again.
7. **Authority ends with the session.** A member domain is authoritative only
   while it holds a live session; when the session ends its incarnation is
   recorded dead and its claims stop being current.
8. **Heterogeneity is the norm.** No fixed cluster, pod, rack, link or gateway
   counts; no assumed topology shape; no vendor assumption anywhere.
9. **Typed outcomes.** Never a bare `false`. Every refusal has a stable code and
   bounded factors.
10. **Determinism.** Composition is a pure function of its inputs. The same
    inputs produce a byte-identical site on any platform and in any arrival
    order.

## Architecture

```
    member domain processes                    site controller process
    +---------------------+                    +-------------------------+
    | cluster / pod /     |   framed TCP       |  authority registry     |
    | rack / shared       |   loopback         |  (epoch, incarnation,   |
    | resource publishers | <----------------> |   generation, attempts, |
    +---------------------+                    |   tombstones)           |
                                               |                         |
                                               |  publication registry   |
                                               |         |               |
                                               |         v               |
                                               |  SiteComposer (pure)    |
                                               |         |               |
                                               |         v               |
                                               |  SnapshotChain          |
                                               |         |               |
                                               |         v               |
                                               |  SiteStore (persistence)|
                                               +-------------------------+
```

* `SiteComposer` reads nothing but its input. No clock, no socket, no file.
* `AuthorityRegistry` is one mutex over small maps. It has no other lock.
* `SnapshotChain` is one mutex over a bounded deque.
* `SiteController` owns all three and never holds two of their locks at once.

## The site model

A **member domain** is `(kind, id)` where kind is CLUSTER, POD, RACK or
SHARED_RESOURCE. It publishes a **declaration**: a sealed, canonical set of
claims about its own subtree, with a generation and a content digest.

Claims are typed: `ClusterClaim`, `PodClaim`, `RackClaim`,
`SharedLinkClaim`, `GatewayClaim`, `CapacityContribution`,
`FailureDomainClaim`, `MaintenanceZone`, `ProtectedObligation`. Each carries
its own record generation and evidence.

A declaration is **sealed**: every list is sorted into canonical order, exact
duplicates are collapsed, and the SHA-256 of the canonical encoding becomes the
declaration digest. Sealing is idempotent and independent of the order the
publisher built its lists in.

A **publication record** is a declaration plus the authority it arrived under:
the controller, the site, the observed epoch, the publisher, the process
incarnation, the publish attempt, the acceptance sequence and the receive
instant.

## Composition

`SiteComposer::compose` runs these stages in order. The order is the contract.

1. **Header and expectation validation.** Bounds, identities, the evaluation
   instant. `INVALID` and `LIMIT_EXCEEDED` mean nothing usable was produced and
   the output is untouched.
2. **Per-domain admission.** Exactly one record per member domain: highest
   generation, then highest attempt, then highest acceptance sequence. A domain
   is CURRENT only when its site, schema, epoch, digest and evidence all agree
   and its expectation is satisfied. Anything else is STALE, SUPERSEDED,
   CONFLICTING, INCOMPATIBLE, RETIRED, REVOKED, REPORTED or ABSENT — each with
   its own status.
3. **Resource attribution.** Claims are grouped by resource key. One distinct
   content across all attestors means the resource is attributed and counted
   once. More than one means CONFLICTING (OWNERSHIP_OVERLAP for exclusive
   resources, ACCOUNTING_CONFLICT for shared ones) and the resource contributes
   nothing to any aggregate.
4. **Capacity ledger.** Every attributed capacity-bearing resource becomes one
   ledger entry with its scope, its channel mask and its attestors.
5. **Failure-domain forest.** Nesting, class agreement, parent agreement, cycles
   and dangling parents.
6. **Maintenance effects.** Zones remove capacity from availability, never from
   the total. A zone whose scope overlaps a protected obligation is refused.
7. **Obligation verdicts.** Earned from available capacity and authoritative
   connectivity only.
8. **Membership and deficits.** Expected-but-absent is a finding.
9. **Lifecycle.** One verdict, by fixed precedence: CONFLICTING, PARTITIONED,
   INCOMPLETE, INDETERMINATE, DEGRADED, CURRENT.
10. **Decisions and provenance.** Every site-level decision records the exact
    `SourceRef` set it was derived from.

**Determinism.** Two compositions of the same input produce the same
`ComposedSite::compute_digest()`, regardless of the order the publications
arrived in. The property tests assert this over randomised sites and the
benchmarks re-check it under shuffled arrival order.

**Partial invalidation.** `invalidate_for_source(site, changed)` returns exactly
the decisions whose source set contains that edge, and the decisions that do
not. A change to a different generation of the same domain invalidates nothing.
This is asserted directly, not inferred.

## Capacity

A capacity channel is either known or unknown. There is no third state and no
default of zero.

Each ledger entry feeds a documented subset of channels:

| scope | ingress | egress | internal |
| --- | --- | --- | --- |
| SITE_INGRESS | yes | | |
| SITE_EGRESS | | yes | |
| SITE_INTERNAL / RACK_LOCAL / POD_LOCAL / CLUSTER_LOCAL | | | yes |
| SHARED_LINK, external | yes | yes | |
| SHARED_LINK, internal | | | yes |
| GATEWAY | yes | yes | |

The ledger must satisfy, channel by channel:

```
total     == sum of every counted entry's contribution to that channel
available + excluded == total
```

`closes_exactly` is *computed*, never asserted. Both sums are checked arithmetic
against a plausibility bound; a sum that would exceed it is
`CAPACITY_OVERFLOW`, the channel becomes unknown, and the ledger reports that it
does not close. `available` is computed by subtraction and independently
recomputed by summing the non-excluded entries; the two must agree.

An empty contribution set aggregates to UNKNOWN, not to zero. A site that
reported nothing has not reported an empty site.

## Failure domains

Two classes, never conflated:

* **PHYSICAL** — the hardware really does share a fate.
* **ADMINISTRATIVE** — an operator decided to treat these resources as one blast
  radius.

Domains nest. Ancestry is resolved to a bounded depth; a cycle is reported as
`FAILURE_DOMAIN_CYCLE` and the affected nodes keep an empty ancestry rather than
a fabricated one. A parent that no member domain declares is
`FAILURE_DOMAIN_DANGLING_PARENT`. Two attestors that disagree about the class or
the parent produce `FAILURE_DOMAIN_CONFLICT`. Two attestors that agree have
their member lists unioned.

`FailureDomainForest::are_separate` answers true only when the two resources are
provably in different domains at every level. Missing evidence makes it false:
separateness must be proven, not assumed.

## Maintenance and protected obligations

A maintenance zone has a state (UNKNOWN, SCHEDULED, ACTIVE, COMPLETE,
CANCELLED) and a scope: named failure domains with their descendants, plus named
resources.

* **ACTIVE** removes its scope from availability and leaves the total alone. The
  site still owns the hardware; it is simply not for sale.
* **UNKNOWN** does the same, conservatively: an operator who has not said
  whether a zone is active has not said the capacity is usable. The zone's
  status and the site lifecycle both report the uncertainty.
* **SCHEDULED / COMPLETE / CANCELLED** exclude nothing.

A zone whose scope overlaps a **protected obligation** is refused with
`MAINTENANCE_CONFLICT` rather than applied. An obligation with no scope is
site-wide and never blocks maintenance: it is simply evaluated against the
capacity that remains, and may then be violated. That outcome is reported, not
prevented.

## Authority and fencing

Four independent fences are applied to every mutation, each with its own status:

* **epoch** — a lower epoch than the site has observed is `FENCED_EPOCH`
  forever.
* **incarnation** — this logical holder is a different process lifetime:
  `FENCED_INCARNATION`. Sequences are *issued* by the registry, never guessed: a
  caller with no sequence asks for one and is given the domain's next.
* **generation** — the holder already advanced past the offered generation:
  `FENCED_GENERATION`. A generation is immutable: the same generation with
  different content is refused.
* **attempt** — the exact publication was already delivered: `FENCED_ATTEMPT`
  or `REPLAYED`.

On top of the fences sits a **tombstone table**. When a session ends, the
incarnation that held it is recorded dead permanently. A process that restarts
and replays its previous incarnation is refused even though every other field it
presents is plausible. A genuinely new process is admitted with the next
sequence. The death record is written to the store, so it survives the
controller's own restart.

**Stale controllers cannot publish.** `SnapshotChain::publish` keeps the epoch,
incarnation and generation of the last snapshot it accepted and refuses
anything that does not strictly advance one of them. A restarted controller
takes the next epoch and publishes immediately, so the term it replaced is
fenced at once rather than at the first member publication.

## Snapshots

A snapshot is an immutable publication of one composed site: the assigned
sequence, the site digest, the envelope digest, the lifecycle, the source set
and the full composed state. The chain assigns the sequence, recomputes the
envelope digest, verifies the state digest, and keeps a bounded history.
Republication of identical content at the same generation is
`ALREADY_EXISTS` and does not advance the sequence; a *different* content at the
same generation is `FENCED_GENERATION`.

## Persistence and recovery

The store is an append-only sequence of self-describing records.

```
file header, 48 bytes
  0..7    magic "SFABSTOR"
  8..11   format version
  12..15  reserved, must be zero
  16..47  SHA-256 of bytes 0..15

record header, 80 bytes
  0..3    magic "SFRC"
  4..5    record type
  6..7    flags
  8..11   payload length
  12..15  reserved, must be zero
  16..47  SHA-256 of the payload
  48..79  SHA-256 of bytes 0..47
payload, exactly the declared length
```

Recovery reads forward and stops at the first record that does not verify. What
it stops on decides the report: a header that does not fit is a torn tail, a
digest that disagrees is corruption, a payload shorter than declared is a torn
tail. A torn tail is cut back to the last good record so the next append starts
clean; corruption in the middle is *not* cut away, because it is evidence. A
file whose header is unreadable, or whose format version is not this build's, is
refused outright.

The store is bounded in both bytes and records, and the bounds are enforced
before anything is written. Compaction keeps the newest snapshots and the newest
publication per member domain.

## Wire protocol and transport

Frames are length-prefixed and self-checking:

```
magic u32 'SFAB' | version u16 | message type u16 | flags u32
payload length u32 | SHA-256 of the payload (32 bytes) | payload
```

A reader validates the magic, the version, the type and the length *before* it
allocates, then the digest, then decodes. A frame whose declared length exceeds
the configured maximum is refused without a buffer of that size ever existing.

Messages: HELLO, PUBLISH, RETIRE, FETCH_SITE, HEARTBEAT, INVALIDATE, REVOKE,
ERROR, SHUTDOWN, PING/PONG.

The transport is blocking loopback TCP with an explicit receive timeout. Closing
a listener from another thread interrupts an in-flight accept, which is how
shutdown is signalled; a worker blocked on a socket wakes on a short header poll
and observes the stop flag. This is a real socket: the multiprocess tests run
separate operating-system processes against it over 127.0.0.1.

## Concurrency and ownership audit

The audit was performed against the code, not against an intention. Each row is
a property of the current implementation and most are exercised by
`test_concurrency.cpp` or the multiprocess suite.

| Hazard | How it is excluded |
| --- | --- |
| Self-deadlock | `SiteStore::compact_locked` exists precisely so that `open`, which holds the store mutex, never calls the locking `compact`. |
| Mutex re-entry | No entry point calls another entry point of the same object while holding its lock. |
| Lock inversion | There is no path that holds two locks. `SiteController` takes the registry lock, releases it, and only then touches the authority, the chain or the store, each of which has its own leaf mutex. |
| Read-to-write upgrade | Composition copies its inputs under the registry lock into a `CompositionInput`, releases, and composes on the copy. There is no shared-then-exclusive upgrade anywhere. |
| Callbacks under locks | No user-supplied function is invoked under any lock. The clock is called before locks are taken. |
| Joining workers while they need held state | Handlers set an atomic done flag without taking the handlers mutex, so the joiner can hold that mutex while joining. |
| Shutdown races | Closing the listener interrupts `accept`; the queue wait is notified; socket reads use a 200 ms header poll and observe the stop flag. Stop is idempotent and safe to call twice, from any thread. |
| Stale completion | A handler that finishes after the accept loop reaped it is joined by the reaper before its entry is erased; a finished handler never writes to freed state. |
| Iterator/reference invalidation | Composed state is built into a local and moved to the output only on success. Attestation pointers point into the caller's immutable input, which outlives composition. |
| Use-after-free | No raw owning pointers. The store, the chain, the registry and the controller all hold their state behind `unique_ptr`. |
| Resource leaks | Sockets are owned by RAII handles; handlers are joined on every path, including a failed start. `Threads` are joined in `stop`, and `stop` is called by the destructor. |
| Inconsistent lock order | There is exactly one ordering: registry, then authority/chain/store, never the reverse, and never two of the latter at once. |

Additional bounds that keep the audit meaningful: at most
`max_connections` member sessions are served at once (one handler thread each,
reaped as they finish); a peer above the bound is refused rather than queued.

## Building

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Or directly:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Presets: `release`, `debug`, `asan`. The build is warning-clean with
`/W4 /permissive- /WX` on MSVC and `-Wall -Wextra -Wpedantic -Wconversion
-Wsign-conversion -Wshadow -Werror` elsewhere. The warning interface is
`BUILD_INTERFACE`-scoped, so consumers of the installed package never inherit
first-party build flags.

## Installing and consuming

```sh
cmake --install build/release --prefix /some/prefix
```

```cmake
find_package(site_fabric 1.0 REQUIRED)
target_link_libraries(my_consumer PRIVATE site_fabric::site_fabric)
```

The installed package exports `site_fabric::site_fabric`, the public headers,
the three tools, and the license documents.

## Tools

### `site_fabric_controller`

```
site_fabric_controller --port N [--host H] [--site S] [--id NAME]
                       [--store PATH] [--expect KIND:ID]...
                       [--stop-file PATH] [--run-ms N] [--print] [--quiet]
```

Holds the site lease, accepts member publications over loopback TCP, composes on
every accepted publication, publishes a snapshot, and writes a store when
`--store` is given. It stops when `--run-ms` elapses or the `--stop-file`
appears; there is no signal handler and no internal timeout, so an operator
interrupt is a hard kill — which is exactly what the store is built to recover
from.

### `site_fabric_member`

```
site_fabric_member --port N [--host H] [--site S] --domain KIND:ID
                   [--publisher NAME] [--boot-nonce NONCE] [--sequence N]
                   [--capacity-bps N] [--ttl-ms N] [--hold-ms N]
                   [--scope ingress|egress|internal|gateway]
                   [--schema-major N] [--generation N] [--quiet]
```

Publishes one member domain's declaration. Exit codes: 0 published, 2 usage,
3 refused locally, 4 transport or handshake refused, 5 publication refused.

### `site_fabric_inspect`

```
site_fabric_inspect scenario  [--seed N] [--clusters N] [--pods N] [--racks N]
                              [--links N] [--gateways N] [--domains N] [--json]
site_fabric_inspect store     PATH
site_fabric_inspect controller [--host H] --port N [--site S]
site_fabric_inspect versions
```

Read-only. It never publishes anything.

## Library API sketch

```cpp
#include <site_fabric/site_fabric.hpp>

// Compose a site. No clock, no socket, no file.
site_fabric::SiteComposer composer;
site_fabric::ComposedSite site;
const site_fabric::Status status = composer.compose(input, site);
if (!site_fabric::is_ok(status)) { /* nothing usable was produced */ }
// site.lifecycle, site.members, site.ownership, site.capacity,
// site.failure_domains, site.maintenance, site.obligations,
// site.conflicts, site.deficits, site.decisions
const site_fabric::Digest identity = site.compute_digest();

// Ask what a change invalidates.
const site_fabric::InvalidationReport report =
    site_fabric::invalidate_for_source(site, changed_source);

// Run a controller and publish to it.
site_fabric::SiteController::create(config, controller);
controller->start();
site_fabric::MemberPublisher::create(publisher_config, publisher);
publisher->connect();
publisher->publish(declaration, site_fabric::PublishAttempt{1, 1}, response);
```

## Tests

```sh
build/release/tests/site_fabric_tests.exe            # 138 unit tests
build/release/tests/site_fabric_tests.exe --repeat 3 # the same 138, three times
build/release/tests/site_fabric_multiprocess_tests.exe   # 4 multiprocess tests
```

No test carries a timeout, and no test is wrapped in one. A hang is a defect to
diagnose and fix, never something to terminate or to classify as a pass.

Suites: `core`, `composition`, `capacity`, `failure_domains`,
`maintenance`, `authority`, `invalidation`, `snapshots`, `persistence`,
`protocol`, `recovery`, `concurrency`, `property`, `adversarial`,
`controller`, and the Windows-only `multiprocess` suite.

The property tests are seeded; every run prints the seed it used and the
failing iteration when something breaks, so a failure replays exactly. The
capacity tests compare the composer against an independent reference model in
`tests/test_support.hpp` that shares no code with it, and a second hierarchical
reference recomputes cluster capacity from the racks a cluster claims.

## Benchmarks

`site_fabric_benchmarks` performs complete work rather than a synthetic inner
loop. Representative figures from a Release build on one Windows 11 host
(16 logical cores, MSVC 19.44):

| Benchmark | Work completed | Time |
| --- | --- | --- |
| generate small | 4 declarations, 4 members, 17 owned resources | 0.20 ms |
| compose small | 10 ledger entries, 29 decisions | 0.23 ms |
| compose medium | 13 declarations, 67 owned resources, 48 ledger entries, 79 decisions | 0.55 ms |
| compose large | 41 declarations, 209 owned resources, 158 ledger entries, 221 decisions | 1.47 ms |
| recompose large | shuffled arrival order, identical site digest | 2.17 ms |
| digest large | canonical encoding of 209 resources | 0.34 ms |
| store large | 41 publication records plus a snapshot file | 3.72 ms |
| recover large | read back, verify and decode 41 publications | 4.31 ms |
| authority validate | 200,000 lease evaluations | 18.0 ms |
| encode_frame | 200,000 frames, 11.2 MB framed | 114.5 ms |

These numbers describe this runtime on this machine. They say nothing about a
data centre.

## Limits

Every bound is enforced before allocation or arithmetic. A selection:

| Bound | Value |
| --- | --- |
| Member domains per site | 4096 |
| Expected members | 4096 |
| Distinct resources | 65536 |
| Distinct failure domains | 8192 |
| Failure-domain nesting depth | 32 |
| Capacity plausibility bound | 1 Pbps per channel |
| Evidence validity claim | 7 days |
| Frame | 4 MiB |
| Store record payload | 8 MiB |
| Store size / records | 64 MiB / 65536 (default, configurable) |
| Concurrent member sessions | 64 (default, configurable) |
| Factors per outcome | 64 |

## Project layout

```
include/site_fabric/   the public headers
src/core/              codecs, model primitives, digests, snapshots
src/compose/           validation and the composition engine
src/authority/         leases, fencing, tombstones
src/persistence/       the versioned store
src/protocol/          framing and message codecs
src/transport/         loopback TCP
src/controller/        the site controller
src/publisher/         the member-domain client
src/synthetic/         the deterministic scenario generator
tools/                 controller, member and inspection tools
examples/              quick start and a loopback session
benchmarks/            completed-work benchmarks
tests/                 the test suites
```

## Limitations

These are real and deliberate.

* **One site, one controller.** Site Fabric composes one physical site. It has
  no opinion about other sites and does not exchange anything with them.
* **No authentication.** Lease tokens are structural: they carry a site, epoch,
  incarnation, generation and lease id, and are compared against the registry.
  They are not signed and there is no transport authentication. The transport is
  loopback; a deployment that exposes the port must add its own protection.
* **The multiprocess suite is Windows-only.** The library's transport has a
  POSIX path, but the test harness that starts and kills independent processes
  is written against the Win32 API, and it is only built and run on Windows
  here. The POSIX transport path is therefore compiled but not proved by the
  multiprocess tests in this repository.
* **AddressSanitizer required its runtime on PATH.** With MSVC
  `/fsanitize=address` the runtime is a DLL
  (`clang_rt.asan_dynamic-x86_64.dll`); the test executables need its directory
  on `PATH`. That is documented here rather than hidden.
* **Aliasing is undetectable in general.** Two member domains that describe the
  same physical capacity under two different resource keys are two ledger
  entries. Site Fabric de-duplicates by resource key and by attestor; it cannot
  know that `SHARED_LINK:uplink-7` and `CAPACITY_POOL:pool-3` are the same wire.
* **Capacity units are bits per second.** No unit conversion, no byte/burst
  accounting, no time-of-day profiles.
* **Composition is periodic, not streaming.** A site is recomposed when
  something is accepted or asked for. There is no incremental evaluation.
* **The store is one file.** No sharding, no log-structured merge, no
  replication. Compaction rewrites it atomically.
* **No real hardware or protocol behaviour is claimed.** Nothing here talks to a
  switch, a NIC, an ASIC or a vendor SDK. The benchmark and test figures describe
  the runtime, not a network. The synthetic generator produces SYNTHETIC fixture
  data and says so in its own output.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
