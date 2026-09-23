// Bounded TCP transport for the control-fabric protocol.
//
// Concurrency model: one accept thread plus a fixed pool of worker threads fed
// by a bounded connection queue. The connection bound is enforced before a
// socket is queued, so memory and thread use are bounded by configuration.
// Shutdown never waits on a timeout: the accept loop is woken by a self-connect
// and in-flight connections are unblocked with an explicit socket shutdown.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_TRANSPORT_HPP
#define SNCF_TRANSPORT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sncf/canonical.hpp"
#include "sncf/fabric.hpp"
#include "sncf/protocol.hpp"
#include "sncf/service.hpp"

namespace sncf {

struct ServerStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_refused = 0;
  std::uint64_t connections_completed = 0;
  std::uint64_t requests_served = 0;
  std::uint64_t requests_refused = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;

  [[nodiscard]] Value to_value() const;
};

class Server {
 public:
  struct Options {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0;  // 0 selects an ephemeral port
    std::size_t max_connections = 8;
    std::size_t worker_threads = 4;
    std::size_t max_frame_bytes = kDefaultMaxFrameBytes;
  };

  [[nodiscard]] static Result<std::unique_ptr<Server>> start(Fabric& fabric, Options options) noexcept;

  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] ServerStats stats() const noexcept;

  /// True once a client has successfully requested shutdown.
  [[nodiscard]] bool shutdown_requested() const noexcept;

  /// Blocks until a shutdown request has been served. No polling and no
  /// timeout: the condition is signalled by the worker that served the request.
  void wait_for_shutdown() noexcept;

  /// Stops accepting, unblocks every in-flight connection, drains and joins.
  /// Idempotent.
  void stop() noexcept;

 private:
  Server() = default;

  void accept_main() noexcept;
  void worker_main() noexcept;
  void serve_connection(std::intptr_t client) noexcept;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
};

class Client {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Client>> connect(std::string host, std::uint16_t port) noexcept;

  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  /// Sends one request and waits for the matching response. A refusal arrives as
  /// an Error frame and is returned as a Status with the original reason code.
  [[nodiscard]] Result<Value> call(std::string_view operation, Value arguments) noexcept;

  /// Sends raw bytes and reads one frame back. Used by the adversarial tests to
  /// prove that malformed input cannot produce a valid-looking success.
  [[nodiscard]] Result<Value> send_raw_frame(std::string_view bytes) noexcept;

  /// Sends a well-formed request frame whose payload is arbitrary text.
  [[nodiscard]] Result<Value> call_raw_payload(std::string_view payload) noexcept;

  /// Writes raw bytes without reading anything back. Used by the adversarial
  /// tests to inject partial and corrupt frames.
  [[nodiscard]] Result<void> send_bytes(std::string_view bytes) noexcept;

  /// Half-closes the sending side so the peer observes a clean end of stream.
  void shutdown_send() noexcept;

  /// Reads exactly one frame. Returns RefusedTruncatedInput when the peer closed
  /// before a complete frame arrived, which is how a refused connection is
  /// distinguished from a successful one.
  [[nodiscard]] Result<Frame> read_frame() noexcept;

  void close() noexcept;

 private:
  Client() = default;

  std::intptr_t socket_ = -1;
  std::uint64_t next_request_id_ = 1;
  std::string buffer_;
};

}  // namespace sncf

#endif  // SNCF_TRANSPORT_HPP
