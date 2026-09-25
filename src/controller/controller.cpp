// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The site controller.
//
// Concurrency, stated once and then obeyed:
//
//   registry_mutex   guards the accepted publications, retirements, revocations
//                    and the composition generation. It is held only while a
//                    map is read or written.
//   queue_mutex      guards the accepted-connection queue.
//   authority, chain and store each have one internal mutex of their own.
//
// No path holds two of them at once. Composition runs on a copy taken under
// registry_mutex and released before the composer is called, so nothing here
// performs a read-to-write upgrade, invokes a callback under a lock, or joins a
// thread while holding state that thread needs.
//
// Shutdown is cooperative. Closing the listener interrupts accept(); queue
// waiters are notified; a worker blocked on a socket wakes on the short header
// timeout and observes the stop flag. Nothing is killed, and no timer decides
// that a hang is a pass.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/digest.hpp"
#include "internal/crypto.hpp"
#include "site_fabric/controller.hpp"
#include "site_fabric/limits.hpp"
#include "site_fabric/protocol.hpp"
#include "transport/tcp.hpp"

namespace site_fabric {

std::int64_t system_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace {

/// How long a worker waits for the next frame header before re-checking the
/// stop flag. Short enough that shutdown is prompt, long enough that an idle
/// connection costs nothing.
constexpr std::int64_t kHeaderPollMs = 200;

struct Counters {
  std::atomic<std::uint64_t> connections_accepted{0};
  std::atomic<std::uint64_t> connections_refused{0};
  std::atomic<std::uint64_t> frames_received{0};
  std::atomic<std::uint64_t> frames_rejected{0};
  std::atomic<std::uint64_t> publications_accepted{0};
  std::atomic<std::uint64_t> publications_refused{0};
  std::atomic<std::uint64_t> publications_replayed{0};
  std::atomic<std::uint64_t> retirements_accepted{0};
  std::atomic<std::uint64_t> compositions{0};
  std::atomic<std::uint64_t> snapshots_published{0};
  std::atomic<std::uint64_t> snapshots_refused{0};
  std::atomic<std::uint64_t> hello_accepted{0};
  std::atomic<std::uint64_t> hello_refused{0};
  std::atomic<std::uint64_t> heartbeats{0};
  std::atomic<std::uint64_t> recoveries{0};
  std::atomic<std::uint64_t> recovery_rejections{0};

  [[nodiscard]] ControllerStats snapshot() const {
    ControllerStats out;
    out.connections_accepted = connections_accepted.load();
    out.connections_refused = connections_refused.load();
    out.frames_received = frames_received.load();
    out.frames_rejected = frames_rejected.load();
    out.publications_accepted = publications_accepted.load();
    out.publications_refused = publications_refused.load();
    out.publications_replayed = publications_replayed.load();
    out.retirements_accepted = retirements_accepted.load();
    out.compositions = compositions.load();
    out.snapshots_published = snapshots_published.load();
    out.snapshots_refused = snapshots_refused.load();
    out.hello_accepted = hello_accepted.load();
    out.hello_refused = hello_refused.load();
    out.heartbeats = heartbeats.load();
    out.recoveries = recoveries.load();
    out.recovery_rejections = recovery_rejections.load();
    return out;
  }
};

struct ConnectionSession {
  bool hello_done = false;
  MemberDomainKey domain;
  Incarnation incarnation;
  MemberId publisher;
};

[[nodiscard]] Status send_error(internal::TcpConnection& connection, Status code,
                                std::string detail, std::int64_t timeout_ms) {
  ErrorResponse error;
  error.status = code;
  error.detail = std::move(detail);
  std::vector<std::uint8_t> payload;
  const Status encoded = encode_message(error, payload);
  if (!is_ok(encoded)) {
    return encoded;
  }
  return internal::send_frame(connection,
                              static_cast<std::uint16_t>(MessageType::ERROR_RESPONSE), payload,
                              timeout_ms);
}

template <class Message>
[[nodiscard]] Status send_message(internal::TcpConnection& connection, const Message& message,
                                  std::int64_t timeout_ms) {
  std::vector<std::uint8_t> payload;
  const Status encoded = encode_message(message, payload);
  if (!is_ok(encoded)) {
    return encoded;
  }
  return internal::send_frame(connection, static_cast<std::uint16_t>(message_type_of(message)),
                              payload, timeout_ms);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct SiteController::Impl {
  SiteController* owner = nullptr;
  ControllerConfig config;
  Clock clock;
  AuthorityRegistry authority;
  SnapshotChain chain;
  std::unique_ptr<SiteStore> store;
  SiteComposer composer;
  Counters counters;

  mutable std::mutex registry_mutex;
  std::map<MemberDomainKey, MemberPublicationRecord> publications;
  std::vector<MemberRetirementRecord> retirements;
  std::vector<MemberDomainKey> revoked;
  std::vector<MemberDomainKey> reconstructed;
  std::uint64_t acceptance_sequence = 0;
  Generation site_generation;

  AuthorityToken token;
  Incarnation incarnation;
  Epoch epoch;

  internal::TcpListener listener;
  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::thread accept_thread;

  /// One entry per live member session. The done flag lets the accept loop
  /// reap finished handlers without a lock on the handler side, so a handler
  /// never waits on the lock that is waiting to join it.
  struct Handler {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::mutex handlers_mutex;
  std::vector<Handler> handlers;
  std::size_t active_connections = 0;

  [[nodiscard]] std::int64_t now() const { return clock ? clock() : system_now_ms(); }

  [[nodiscard]] Status persist_publication(const MemberPublicationRecord& record) {
    if (store == nullptr) {
      return Status::OK;
    }
    return store->append_publication(record);
  }

  // --- The accept loop -----------------------------------------------------

  /// Joins handlers that have already finished. Called with handlers_mutex
  /// held, so the handlers it joins never take that lock themselves.
  void reap_finished_handlers_locked() {
    for (auto entry = handlers.begin(); entry != handlers.end();) {
      if (!entry->done->load()) {
        ++entry;
        continue;
      }
      if (entry->thread.joinable()) {
        entry->thread.join();
      }
      entry = handlers.erase(entry);
    }
  }

  void accept_loop() {
    while (running.load()) {
      internal::TcpConnection connection;
      const Status accepted = listener.accept(connection);
      if (!is_ok(accepted)) {
        if (accepted == Status::BUSY) {
          continue;
        }
        // The listener was closed underneath us. That is how shutdown is
        // signalled; it is not an error.
        break;
      }

      const std::lock_guard<std::mutex> guard(handlers_mutex);
      reap_finished_handlers_locked();
      if (active_connections >= config.max_connections) {
        counters.connections_refused.fetch_add(1);
        connection.close();
        continue;
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      Handler entry;
      entry.done = done;
      entry.thread = std::thread([this, connection = std::move(connection), done]() mutable {
        handle_connection(connection);
        done->store(true);
      });
      handlers.push_back(std::move(entry));
      ++active_connections;
      counters.connections_accepted.fetch_add(1);
    }
  }

  /// Joins every live handler. Handlers wake on the short header poll and
  /// observe the stop flag, so this is a cooperative wait, not a kill.
  void join_handlers() {
    const std::lock_guard<std::mutex> guard(handlers_mutex);
    for (auto& entry : handlers) {
      if (entry.thread.joinable()) {
        entry.thread.join();
      }
    }
    handlers.clear();
    active_connections = 0;
  }

  // --- One connection ------------------------------------------------------

  void handle_connection(internal::TcpConnection& connection) {
    ConnectionSession session;
    while (running.load()) {
      std::uint16_t message_type = 0;
      std::vector<std::uint8_t> payload;
      const Status received = internal::receive_frame(connection, message_type, payload,
                                                      kHeaderPollMs, config.io_timeout_ms);
      if (received == Status::BUSY) {
        continue;
      }
      if (!is_ok(received)) {
        if (received != Status::STOPPED) {
          counters.frames_rejected.fetch_add(1);
        }
        break;
      }
      counters.frames_received.fetch_add(1);

      const MessageType type = static_cast<MessageType>(message_type);
      Status handled = Status::OK;
      switch (type) {
        case MessageType::PING:
          handled = respond_ping(connection, payload);
          break;
        case MessageType::HELLO_REQUEST:
          handled = respond_hello(connection, payload, session);
          break;
        case MessageType::PUBLISH_REQUEST:
          handled = respond_publish(connection, payload, session);
          break;
        case MessageType::RETIRE_REQUEST:
          handled = respond_retire(connection, payload, session);
          break;
        case MessageType::FETCH_SITE_REQUEST:
          handled = respond_fetch_site(connection, payload);
          break;
        case MessageType::HEARTBEAT_REQUEST:
          handled = respond_heartbeat(connection, payload, session);
          break;
        default:
          handled = send_error(connection, Status::UNSUPPORTED, "unexpected message type",
                               config.io_timeout_ms);
          break;
      }
      if (!is_ok(handled)) {
        break;
      }
    }

    // The session ended. Two things follow, and both matter.
    //
    // First, whatever authority this incarnation held ends with it, so a
    // process that restarts and replays the same incarnation is refused even
    // though every other field it presents is plausible.
    //
    // Second, a member domain is authoritative only while it holds a live
    // session. The declaration it published is kept, but it stops being
    // current until the domain reports again: an absent member must not keep
    // counting toward a site verdict.
    if (session.hello_done) {
      const std::int64_t now = this->now();
      const Status retired = authority.retire_member_incarnation(
          session.domain, session.incarnation, now, "session ended");
      if (!is_ok(retired)) {
        // Nothing to undo: the tombstone is written regardless, because a
        // failure to record a death must not leave the authority alive.
      }
      if (store != nullptr) {
        const std::string tombstone = session.domain.to_string() + "/" +
                                      session.incarnation.to_string();
        const Status persisted = store->append_tombstone(tombstone);
        if (!is_ok(persisted)) {
          counters.recovery_rejections.fetch_add(1);
        }
      }
      {
        const std::lock_guard<std::mutex> guard(registry_mutex);
        if (std::find(reconstructed.begin(), reconstructed.end(), session.domain) ==
            reconstructed.end()) {
          reconstructed.push_back(session.domain);
        }
      }
      SnapshotPublishResult published;
      const Status composed = owner->recompose(published);
      if (!is_ok(composed)) {
        counters.snapshots_refused.fetch_add(1);
      }
    }
    connection.close();
  }

  [[nodiscard]] Status respond_ping(internal::TcpConnection& connection,
                                    const std::vector<std::uint8_t>& payload) {
    PingMessage request;
    const Status decoded = decode_message(payload, request);
    if (!is_ok(decoded)) {
      return send_error(connection, decoded, "ping", config.io_timeout_ms);
    }
    std::vector<std::uint8_t> response;
    const Status encoded = encode_message(request, response);
    if (!is_ok(encoded)) {
      return encoded;
    }
    return internal::send_frame(connection, static_cast<std::uint16_t>(MessageType::PONG),
                                response, config.io_timeout_ms);
  }

  [[nodiscard]] Status respond_hello(internal::TcpConnection& connection,
                                     const std::vector<std::uint8_t>& payload,
                                     ConnectionSession& session) {
    HelloRequest request;
    Status status = decode_message(payload, request);
    if (!is_ok(status)) {
      return send_error(connection, status, "hello", config.io_timeout_ms);
    }
    if (!(request.site == config.site)) {
      counters.hello_refused.fetch_add(1);
      HelloResponse response;
      response.status = Status::SITE_MISMATCH;
      response.site = config.site;
      response.epoch = epoch;
      response.controller = incarnation;
      response.factors.add("site", request.site.value());
      return send_message(connection, response, config.io_timeout_ms);
    }
    if (request.observed_epoch > epoch) {
      counters.hello_refused.fetch_add(1);
      HelloResponse response;
      response.status = Status::EPOCH_MISMATCH;
      response.site = config.site;
      response.epoch = epoch;
      response.controller = incarnation;
      response.factors.add("observed_epoch", request.observed_epoch.to_string());
      return send_message(connection, response, config.io_timeout_ms);
    }

    std::uint64_t assigned = 0;
    const std::int64_t now = this->now();
    const Status admitted = authority.admit_member_incarnation(
        request.domain, request.incarnation, now, &assigned);
    if (!is_ok(admitted) && admitted != Status::ALREADY_EXISTS) {
      counters.hello_refused.fetch_add(1);
      HelloResponse response;
      response.status = admitted;
      response.site = config.site;
      response.epoch = epoch;
      response.controller = incarnation;
      return send_message(connection, response, config.io_timeout_ms);
    }

    counters.hello_accepted.fetch_add(1);
    session.hello_done = true;
    session.domain = request.domain;
    session.incarnation = Incarnation(assigned, request.incarnation.boot);
    session.publisher = request.publisher;

    // A member that reports live is no longer only a recovered record.
    {
      const std::lock_guard<std::mutex> guard(registry_mutex);
      reconstructed.erase(
          std::remove(reconstructed.begin(), reconstructed.end(), request.domain),
          reconstructed.end());
    }

    (void)status;
    HelloResponse response;
    response.status = Status::OK;
    response.site = config.site;
    response.epoch = epoch;
    response.controller = incarnation;
    response.assigned_sequence = assigned;
    response.current_generation = site_generation;
    return send_message(connection, response, config.io_timeout_ms);
  }

  [[nodiscard]] Status respond_publish(internal::TcpConnection& connection,
                                       const std::vector<std::uint8_t>& payload,
                                       const ConnectionSession& session) {
    PublishRequest request;
    const Status decoded = decode_message(payload, request);
    if (!is_ok(decoded)) {
      return send_error(connection, decoded, "publish", config.io_timeout_ms);
    }
    if (!session.hello_done) {
      counters.publications_refused.fetch_add(1);
      PublishResponse response;
      response.status = Status::UNAUTHORIZED;
      response.site = config.site;
      response.epoch = epoch;
      response.factors.add("hello", "not_completed");
      return send_message(connection, response, config.io_timeout_ms);
    }
    PublishResponse response;
    const Status applied = owner->publish(session.publisher, session.incarnation,
                                          request.declaration, request.attempt, response);
    if (!is_ok(applied)) {
      return send_error(connection, applied, "publish", config.io_timeout_ms);
    }
    return send_message(connection, response, config.io_timeout_ms);
  }

  [[nodiscard]] Status respond_retire(internal::TcpConnection& connection,
                                      const std::vector<std::uint8_t>& payload,
                                      const ConnectionSession& session) {
    RetireRequestMessage request;
    const Status decoded = decode_message(payload, request);
    if (!is_ok(decoded)) {
      return send_error(connection, decoded, "retire", config.io_timeout_ms);
    }
    if (!session.hello_done || !(request.request.domain == session.domain)) {
      RetireResponse response;
      response.status = Status::UNAUTHORIZED;
      response.site = config.site;
      response.epoch = epoch;
      response.factors.add("session", "does_not_match_domain");
      return send_message(connection, response, config.io_timeout_ms);
    }
    RetireResponse response;
    const Status applied = owner->retire(request.request, response);
    if (!is_ok(applied)) {
      return send_error(connection, applied, "retire", config.io_timeout_ms);
    }
    return send_message(connection, response, config.io_timeout_ms);
  }

  [[nodiscard]] Status respond_fetch_site(internal::TcpConnection& connection,
                                          const std::vector<std::uint8_t>& payload) {
    FetchSiteRequest request;
    const Status decoded = decode_message(payload, request);
    if (!is_ok(decoded)) {
      return send_error(connection, decoded, "fetch_site", config.io_timeout_ms);
    }
    FetchSiteResponse response;
    if (!(request.site == config.site)) {
      response.status = Status::SITE_MISMATCH;
      return send_message(connection, response, config.io_timeout_ms);
    }
    SiteSnapshot snapshot;
    const bool found = request.sequence == 0 ? chain.current(snapshot)
                                             : chain.at_sequence(request.sequence, snapshot);
    if (!found) {
      response.status = Status::NOT_FOUND;
      response.factors.add("sequence", std::to_string(request.sequence));
      return send_message(connection, response, config.io_timeout_ms);
    }
    response.status = Status::OK;
    response.has_snapshot = true;
    response.snapshot = std::move(snapshot);
    return send_message(connection, response, config.io_timeout_ms);
  }

  [[nodiscard]] Status respond_heartbeat(internal::TcpConnection& connection,
                                         const std::vector<std::uint8_t>& payload,
                                         const ConnectionSession& session) {
    HeartbeatRequest request;
    const Status decoded = decode_message(payload, request);
    if (!is_ok(decoded)) {
      return send_error(connection, decoded, "heartbeat", config.io_timeout_ms);
    }
    counters.heartbeats.fetch_add(1);
    HeartbeatResponse response;
    response.epoch = epoch;
    response.site_generation = site_generation;
    SiteSnapshot snapshot;
    if (chain.current(snapshot)) {
      response.site_sequence = snapshot.sequence;
    }
    if (!session.hello_done) {
      response.status = Status::UNAUTHORIZED;
      response.factors.add("hello", "not_completed");
      return send_message(connection, response, config.io_timeout_ms);
    }
    if (!authority.member_incarnation_live(session.domain, session.incarnation)) {
      response.status = Status::FENCED_INCARNATION;
      return send_message(connection, response, config.io_timeout_ms);
    }
    response.status = Status::OK;
    const Status listed = owner->changed_since(request.since_sequence, response.invalidated);
    if (!is_ok(listed)) {
      response.status = listed;
    }
    return send_message(connection, response, config.io_timeout_ms);
  }
};

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

SiteController::SiteController(const ControllerConfig& config)
    : impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
  impl_->config = config;
  impl_->clock = config.clock ? config.clock : Clock(&system_now_ms);
  impl_->chain.set_history_limit(config.snapshot_history);
  impl_->site_generation = Generation::unset();
  impl_->incarnation = config.incarnation;
  if (!impl_->incarnation.boot.valid()) {
    impl_->incarnation.boot = BootNonce(internal::random_hex(16));
  }
  if (impl_->incarnation.sequence == 0) {
    impl_->incarnation.sequence = 1;
  }
}

SiteController::~SiteController() {
  if (impl_ != nullptr) {
    stop();
  }
}

Status SiteController::create(const ControllerConfig& config,
                              std::unique_ptr<SiteController>& out) {
  if (!config.site.valid()) {
    return Status::INVALID;
  }
  if (config.lease_ttl_ms <= 0) {
    return Status::INVALID;
  }
  if (config.max_connections == 0 || config.max_connections > limits::kMaxConnections) {
    return Status::INVALID;
  }
  if (config.snapshot_history > limits::kMaxSnapshotHistory) {
    return Status::INVALID;
  }
  SiteExpectation expectation = config.expectation;
  if (expectation.site.empty()) {
    expectation.site = config.site;
  }
  if (expectation.canonicalize() != Status::OK) {
    return Status::INVALID;
  }
  out = std::unique_ptr<SiteController>(new SiteController(config));
  out->impl_->config.expectation = std::move(expectation);
  return Status::OK;
}

const SiteId& SiteController::site() const noexcept { return impl_->config.site; }
Epoch SiteController::epoch() const { return impl_->epoch; }
Generation SiteController::generation() const { return impl_->site_generation; }
Incarnation SiteController::incarnation() const { return impl_->incarnation; }
std::uint16_t SiteController::port() const { return impl_->listener.port(); }
std::string SiteController::endpoint() const { return impl_->listener.endpoint(); }
bool SiteController::running() const noexcept { return impl_->running.load(); }
ControllerStats SiteController::stats() const { return impl_->counters.snapshot(); }
AuthorityRegistry& SiteController::authority() { return impl_->authority; }
const AuthorityRegistry& SiteController::authority() const { return impl_->authority; }
const SiteExpectation& SiteController::expectation() const { return impl_->config.expectation; }
const ControllerConfig& SiteController::config() const { return impl_->config; }

const StoreRecoveryReport& SiteController::recovery() const {
  static const StoreRecoveryReport kEmpty;
  if (impl_->store == nullptr) {
    return kEmpty;
  }
  return impl_->store->recovery();
}

std::string SiteController::status_text() const {
  const ControllerStats stats = impl_->counters.snapshot();
  std::string out = "controller ";
  out += impl_->config.id.value();
  out += " site=" + impl_->config.site.value();
  out += " epoch=" + impl_->epoch.to_string();
  out += " incarnation=" + impl_->incarnation.to_string();
  out += " generation=" + impl_->site_generation.to_string();
  out += " endpoint=" + impl_->listener.endpoint();
  out += impl_->running.load() ? " running" : " stopped";
  out += " accepted=" + std::to_string(stats.publications_accepted);
  out += " refused=" + std::to_string(stats.publications_refused);
  out += " snapshots=" + std::to_string(stats.snapshots_published);
  return out;
}

Status SiteController::start() {
  if (impl_->running.load()) {
    return Status::ALREADY_EXISTS;
  }
  impl_->stopping.store(false);

  const std::int64_t now = impl_->now();

  // Recovery comes first: what survived on disk decides which epoch this
  // controller takes, and therefore which controller it fences.
  if (impl_->config.persistent) {
    StoreConfig store_config = impl_->config.store;
    if (store_config.path.empty()) {
      return Status::INVALID;
    }
    if (store_config.snapshot_history == 0) {
      store_config.snapshot_history = impl_->config.snapshot_history;
    }
    impl_->store = std::make_unique<SiteStore>();
    const Status opened = impl_->store->open(store_config);
    if (!is_ok(opened)) {
      impl_->counters.recovery_rejections.fetch_add(1);
      impl_->store.reset();
      return opened;
    }
    impl_->counters.recoveries.fetch_add(1);

    StoreContents contents;
    const Status loaded = impl_->store->load(contents);
    const bool recovered = is_ok(loaded) || loaded == Status::TRUNCATED ||
                           loaded == Status::LIMIT_EXCEEDED ||
                           loaded == Status::INTEGRITY_FAILURE;
    if (recovered) {
      if (contents.has_snapshot) {
        const Status restored = impl_->chain.restore(contents.latest_snapshot);
        if (!is_ok(restored)) {
          impl_->counters.recovery_rejections.fetch_add(1);
          impl_->store.reset();
          return restored;
        }
        impl_->epoch = contents.latest_snapshot.epoch;
        impl_->site_generation = contents.latest_snapshot.generation;
      }
      for (const auto& lease : contents.leases) {
        if (lease.kind == AuthorityKind::SITE_CONTROLLER && lease.epoch > impl_->epoch) {
          impl_->epoch = lease.epoch;
        }
      }
      std::uint64_t highest = 0;
      {
        const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
        for (const auto& record : contents.publications) {
          const auto found = impl_->publications.find(record.domain);
          if (found == impl_->publications.end() ||
              found->second.generation < record.generation) {
            impl_->publications[record.domain] = record;
          }
          impl_->reconstructed.push_back(record.domain);
          highest = std::max(highest, record.acceptance_sequence);
        }
        impl_->acceptance_sequence = highest;
        impl_->retirements = contents.retirements;
      }
    }
    impl_->counters.recovery_rejections.fetch_add(contents.recovery.diagnostics.size());
  }

  // A restarted controller takes the next epoch, which is what fences the
  // controller it replaced even if that one is still alive.
  impl_->epoch = impl_->epoch.is_set() ? impl_->epoch.next() : Epoch::initial();

  const Generation lease_generation = impl_->site_generation.is_set()
                                          ? impl_->site_generation
                                          : Generation::initial();
  const Status acquired = impl_->authority.acquire_site_authority(
      impl_->config.id.value(), impl_->config.site, impl_->epoch, impl_->incarnation,
      lease_generation, now, impl_->config.lease_ttl_ms, impl_->token);
  if (!is_ok(acquired)) {
    return acquired;
  }

  const Status bound = internal::TcpListener::listen_on(
      impl_->config.bind_host, impl_->config.bind_port,
      static_cast<int>(impl_->config.accept_backlog), impl_->listener);
  if (!is_ok(bound)) {
    return bound;
  }

  impl_->running.store(true);
  impl_->accept_thread = std::thread([this]() { impl_->accept_loop(); });

  // Publish once immediately. On a fresh site this is the first statement of
  // what the site is; after a restart it is what fences the previous epoch, so
  // the controller that this one replaced cannot publish under the old term
  // even for the moment before the first member reports.
  SnapshotPublishResult published;
  const Status composed = recompose(published);
  if (!is_ok(composed) && composed != Status::NOT_FOUND) {
    // A composition failure at start is not fatal: the controller keeps
    // running and reports the defect on the next publication.
    impl_->counters.snapshots_refused.fetch_add(1);
  }
  return Status::OK;
}

Status SiteController::stop() {
  if (impl_ == nullptr) {
    return Status::OK;
  }
  const bool was_running = impl_->running.exchange(false);
  impl_->stopping.store(true);

  impl_->listener.close();
  if (impl_->accept_thread.joinable()) {
    impl_->accept_thread.join();
  }
  impl_->join_handlers();

  if (impl_->store != nullptr) {
    const Status closed = impl_->store->close();
    if (!is_ok(closed)) {
      impl_->counters.recovery_rejections.fetch_add(1);
    }
  }
  impl_->stopping.store(false);
  return was_running ? Status::OK : Status::OK;
}

Status SiteController::serve(std::atomic<bool>& stop_flag) {
  while (impl_->running.load() && !stop_flag.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  stop();
  return Status::OK;
}

Status SiteController::publish(const MemberId& publisher, const Incarnation& incarnation,
                               const MemberDomainDeclaration& declaration,
                               const PublishAttempt& attempt, PublishResponse& out) {
  out = PublishResponse{};
  out.site = impl_->config.site;
  out.epoch = impl_->epoch;

  if (!declaration.domain.valid() || !(declaration.site == impl_->config.site)) {
    out.status = Status::SITE_MISMATCH;
    impl_->counters.publications_refused.fetch_add(1);
    return out.status;
  }

  MemberDomainDeclaration sealed = declaration;
  const Status sealed_status = sealed.seal();
  if (!is_ok(sealed_status)) {
    out.status = sealed_status;
    impl_->counters.publications_refused.fetch_add(1);
    return out.status;
  }
  Digest offered = sealed.digest;
  if (!declaration.digest.is_zero() && !(declaration.digest == sealed.digest)) {
    // The caller sealed a different model than the one it sent.
    out.status = Status::DIGEST_MISMATCH_DECLARATION;
    out.factors.add("recomputed", sealed.digest.short_hex());
    impl_->counters.publications_refused.fetch_add(1);
    return out.status;
  }

  const std::int64_t now = impl_->now();
  const Status fenced = impl_->authority.fence_publication(
      sealed.domain, declaration.observed_epoch, incarnation, sealed.generation, offered,
      attempt, now);
  if (!is_ok(fenced)) {
    out.status = fenced;
    impl_->counters.publications_refused.fetch_add(1);
    if (fenced == Status::REPLAYED || fenced == Status::FENCED_ATTEMPT) {
      impl_->counters.publications_replayed.fetch_add(1);
    }
    return out.status;
  }

  MemberPublicationRecord record;
  record.domain = sealed.domain;
  record.generation = sealed.generation;
  record.digest = offered;
  record.schema = sealed.schema;
  record.site = sealed.site;
  record.observed_epoch = declaration.observed_epoch;
  record.incarnation = incarnation;
  record.publisher = publisher;
  record.attempt = attempt;
  record.received_at_ms = now;
  record.has_declaration = impl_->config.retain_declarations;
  if (record.has_declaration) {
    record.declaration = sealed;
  }

  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    record.acceptance_sequence = ++impl_->acceptance_sequence;
    impl_->publications[sealed.domain] = record;
    impl_->reconstructed.erase(
        std::remove(impl_->reconstructed.begin(), impl_->reconstructed.end(), sealed.domain),
        impl_->reconstructed.end());
  }

  impl_->authority.record_publication(sealed.domain, declaration.observed_epoch,
                                      sealed.generation, offered, attempt, now);
  const Status persisted = impl_->persist_publication(record);
  if (!is_ok(persisted)) {
    out.factors.add("persist_publication", site_fabric::to_string(persisted));
  }

  impl_->counters.publications_accepted.fetch_add(1);
  out.status = Status::ACCEPTED;
  out.epoch = impl_->epoch;
  out.site_generation = impl_->site_generation;
  out.acceptance_sequence = record.acceptance_sequence;

  if (impl_->config.compose_on_publish) {
    SnapshotPublishResult result;
    const Status composed = recompose(result);
    if (is_ok(composed) && result.accepted()) {
      out.recomposed = true;
      out.site_generation = impl_->site_generation;
    } else if (!is_ok(composed)) {
      out.factors.add("recompose", site_fabric::to_string(composed));
    }
  }
  return out.status;
}

Status SiteController::retire(const RetireRequest& request, RetireResponse& out) {
  out = RetireResponse{};
  out.site = impl_->config.site;
  out.epoch = impl_->epoch;

  if (!request.domain.valid()) {
    out.status = Status::INVALID;
    return out.status;
  }
  const std::int64_t now = impl_->now();
  if (!impl_->authority.member_incarnation_live(request.domain, request.incarnation)) {
    out.status = Status::FENCED_INCARNATION;
    return out.status;
  }

  MemberRetirementRecord record;
  record.domain = request.domain;
  record.generation = request.generation;
  record.incarnation = request.incarnation;
  record.received_at_ms = now;
  record.reason = request.reason;

  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    record.acceptance_sequence = ++impl_->acceptance_sequence;
    impl_->retirements.push_back(record);
    impl_->reconstructed.erase(
        std::remove(impl_->reconstructed.begin(), impl_->reconstructed.end(), request.domain),
        impl_->reconstructed.end());
  }
  impl_->authority.retire_member_incarnation(request.domain, request.incarnation, now,
                                             request.reason);
  if (impl_->store != nullptr) {
    const Status persisted = impl_->store->append_retirement(record);
    if (!is_ok(persisted)) {
      out.factors.add("persist_retirement", site_fabric::to_string(persisted));
    }
    const Status tombstoned = impl_->store->append_tombstone(
        request.domain.to_string() + "/" + request.incarnation.to_string());
    if (!is_ok(tombstoned)) {
      out.factors.add("persist_tombstone", site_fabric::to_string(tombstoned));
    }
  }

  impl_->counters.retirements_accepted.fetch_add(1);
  out.status = Status::ACCEPTED;
  out.acceptance_sequence = record.acceptance_sequence;

  if (impl_->config.compose_on_publish) {
    SnapshotPublishResult result;
    const Status composed = recompose(result);
    if (!is_ok(composed)) {
      out.factors.add("recompose", site_fabric::to_string(composed));
    }
  }
  return out.status;
}

Status SiteController::revoke_member(const MemberDomainKey& domain, std::string reason) {
  if (!domain.valid()) {
    return Status::INVALID;
  }
  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    if (std::find(impl_->revoked.begin(), impl_->revoked.end(), domain) ==
        impl_->revoked.end()) {
      impl_->revoked.push_back(domain);
    }
  }
  (void)reason;
  SnapshotPublishResult result;
  return recompose(result);
}

Status SiteController::changed_since(std::uint64_t since_sequence,
                                     std::vector<MemberDomainKey>& out) const {
  out.clear();
  const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
  for (const auto& [domain, record] : impl_->publications) {
    if (record.acceptance_sequence > since_sequence) {
      out.push_back(domain);
    }
  }
  std::sort(out.begin(), out.end());
  return Status::OK;
}

Status SiteController::recompose(SnapshotPublishResult& out) {
  out = SnapshotPublishResult{};
  const std::int64_t now = impl_->now();

  CompositionInput input;
  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    input.site = impl_->config.site;
    input.epoch = impl_->epoch;
    input.generation = impl_->site_generation.next();
    input.incarnation = impl_->incarnation;
    input.now_ms = now;
    input.expectation = impl_->config.expectation;
    input.reconstructed = impl_->reconstructed;
    input.revoked = impl_->revoked;
    input.retirements = impl_->retirements;
    input.publications.reserve(impl_->publications.size());
    for (const auto& [domain, record] : impl_->publications) {
      (void)domain;
      input.publications.push_back(record);
    }
  }

  ComposedSite composed;
  const Status composed_status = impl_->composer.compose(input, composed);
  impl_->counters.compositions.fetch_add(1);
  if (!is_ok(composed_status)) {
    out.status = composed_status;
    out.factors.add("compose", site_fabric::to_string(composed_status));
    impl_->counters.snapshots_refused.fetch_add(1);
    return composed_status;
  }

  // The lease must carry the generation being published, or the chain will
  // (correctly) refuse a controller that is publishing under an old grant.
  AuthorityToken token;
  const Status acquired = impl_->authority.acquire_site_authority(
      impl_->config.id.value(), impl_->config.site, impl_->epoch, impl_->incarnation,
      input.generation, now, impl_->config.lease_ttl_ms, token);
  if (!is_ok(acquired)) {
    out.status = acquired;
    out.factors.add("authority", site_fabric::to_string(acquired));
    impl_->counters.snapshots_refused.fetch_add(1);
    return acquired;
  }
  impl_->token = token;

  SiteSnapshot snapshot;
  snapshot.site = composed.site;
  snapshot.epoch = composed.epoch;
  snapshot.generation = composed.generation;
  snapshot.incarnation = composed.incarnation;
  snapshot.published_at_ms = now;
  snapshot.lifecycle = composed.lifecycle;
  snapshot.status = composed.status;
  snapshot.complete = composed.membership_complete && composed.lifecycle == SiteLifecycle::CURRENT;
  snapshot.sources = SourceSet{};
  for (const auto& decision : composed.decisions) {
    snapshot.sources.insert(decision.sources);
  }
  snapshot.factors = composed.factors;
  snapshot.state = composed;
  const Status digest_status = snapshot.recompute_digest();
  if (!is_ok(digest_status)) {
    out.status = digest_status;
    impl_->counters.snapshots_refused.fetch_add(1);
    return digest_status;
  }

  const Status published = impl_->chain.publish(snapshot, token, now, out);
  if (!is_ok(published) && published != Status::ALREADY_EXISTS) {
    impl_->counters.snapshots_refused.fetch_add(1);
    return published;
  }
  if (!(published == Status::ALREADY_EXISTS)) {
    {
      const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
      impl_->site_generation = input.generation;
    }
    impl_->counters.snapshots_published.fetch_add(1);

    // The chain owns the sequence, so the published copy is the one that is
    // persisted. Writing the caller's copy would store a snapshot numbered
    // zero that no reader could ever restore.
    SiteSnapshot stamped;
    if (impl_->chain.current(stamped)) {
      snapshot = stamped;
    }
    if (impl_->store != nullptr) {
      const Status persisted = impl_->store->append_snapshot(snapshot);
      if (!is_ok(persisted)) {
        out.factors.add("persist_snapshot", site_fabric::to_string(persisted));
      }
    }
  }
  return published;
}

Status SiteController::compose_now(ComposedSite& out) const {
  CompositionInput input;
  {
    const std::lock_guard<std::mutex> guard(impl_->registry_mutex);
    input.site = impl_->config.site;
    input.epoch = impl_->epoch;
    input.generation = impl_->site_generation.next();
    input.incarnation = impl_->incarnation;
    input.now_ms = impl_->now();
    input.expectation = impl_->config.expectation;
    input.reconstructed = impl_->reconstructed;
    input.revoked = impl_->revoked;
    input.retirements = impl_->retirements;
    input.publications.reserve(impl_->publications.size());
    for (const auto& [domain, record] : impl_->publications) {
      (void)domain;
      input.publications.push_back(record);
    }
  }
  return impl_->composer.compose(input, out);
}

Status SiteController::current_snapshot(SiteSnapshot& out) const {
  if (impl_->chain.current(out)) {
    return Status::OK;
  }
  return Status::NOT_FOUND;
}

Status SiteController::snapshot_at(std::uint64_t sequence, SiteSnapshot& out) const {
  if (impl_->chain.at_sequence(sequence, out)) {
    return Status::OK;
  }
  return Status::NOT_FOUND;
}

Status SiteController::snapshot_sequences(std::vector<std::uint64_t>& out) const {
  out = impl_->chain.sequences();
  return Status::OK;
}

}  // namespace site_fabric
