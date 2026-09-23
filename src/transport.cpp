// Copyright 2026 Summon Software Labs.
#include "sncf/transport.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sncf {
namespace {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

std::once_flag g_socket_layer;
Status g_socket_layer_status = Status(ReasonCode::Accepted);

void initialise_socket_layer() {
#if defined(_WIN32)
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    g_socket_layer_status = Status(ReasonCode::RefusedJournalUnavailable, "Winsock could not be initialised");
  }
#endif
}

Status ensure_socket_layer() {
  std::call_once(g_socket_layer, initialise_socket_layer);
  return g_socket_layer_status;
}

void close_socket(socket_t socket) noexcept {
  if (socket == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

void shutdown_socket(socket_t socket) noexcept {
  if (socket == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

Result<std::size_t> send_all(socket_t socket, std::string_view bytes) noexcept {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const std::size_t chunk = (std::min)(bytes.size() - sent, static_cast<std::size_t>(1U << 16U));
#if defined(_WIN32)
    const int written = ::send(socket, bytes.data() + sent, static_cast<int>(chunk), 0);
#else
    const ssize_t written = ::send(socket, bytes.data() + sent, chunk, 0);
    const int written_int = static_cast<int>(written);
#endif
#if defined(_WIN32)
    const int written_int = written;
#endif
    if (written_int <= 0) {
      return Status(ReasonCode::RefusedJournalUnavailable, "connection write failed");
    }
    sent += static_cast<std::size_t>(written_int);
  }
  return sent;
}

Result<std::size_t> recv_some(socket_t socket, char* buffer, std::size_t capacity) noexcept {
#if defined(_WIN32)
  const int received = ::recv(socket, buffer, static_cast<int>(capacity), 0);
#else
  const ssize_t received = ::recv(socket, buffer, capacity, 0);
#endif
  if (received < 0) {
    return Status(ReasonCode::RefusedJournalUnavailable, "connection read failed");
  }
  return static_cast<std::size_t>(received);
}

Result<socket_t> connect_to(const std::string& host, std::uint16_t port) noexcept {
  auto layer = ensure_socket_layer();
  if (!layer.ok()) {
    return layer;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return Status(ReasonCode::RefusedJournalUnavailable, "address could not be resolved");
  }
  socket_t socket = kInvalidSocket;
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == kInvalidSocket) {
      continue;
    }
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_socket(socket);
    socket = kInvalidSocket;
  }
  ::freeaddrinfo(resolved);
  if (socket == kInvalidSocket) {
    return Status(ReasonCode::RefusedJournalUnavailable, "connection could not be established");
  }
  return socket;
}

Result<socket_t> listen_on(const std::string& address, std::uint16_t port, std::uint16_t& bound_port) noexcept {
  auto layer = ensure_socket_layer();
  if (!layer.ok()) {
    return layer;
  }
  socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(ReasonCode::RefusedJournalUnavailable, "listening socket could not be created");
  }
  const int reuse = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
    close_socket(socket);
    return Status(ReasonCode::RefusedInvalidRange, "bind address is not a valid IPv4 literal");
  }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    close_socket(socket);
    return Status(ReasonCode::RefusedJournalUnavailable, "bind failed");
  }
  if (::listen(socket, static_cast<int>(32)) != 0) {
    close_socket(socket);
    return Status(ReasonCode::RefusedJournalUnavailable, "listen failed");
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int length = sizeof(actual);
#else
  socklen_t length = sizeof(actual);
#endif
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &length) != 0) {
    close_socket(socket);
    return Status(ReasonCode::RefusedJournalUnavailable, "bound port could not be read back");
  }
  bound_port = ntohs(actual.sin_port);
  return socket;
}

constexpr std::size_t kReadChunk = 16U * 1024U;
constexpr std::size_t kMaxBufferedBytes = 4U * 1024U * 1024U;

}  // namespace

struct Server::Impl {
  Fabric* fabric = nullptr;
  std::unique_ptr<FabricService> service;
  Options options;
  socket_t listen_socket = kInvalidSocket;
  std::thread accept_thread;
  std::vector<std::thread> workers;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<socket_t> queue;
  std::set<socket_t> active;
  std::atomic<bool> stopping{false};
  std::mutex teardown_mutex;
  bool accept_closed = false;
  bool broadcast_done = false;
  mutable std::mutex stats_mutex;
  ServerStats stats;
  mutable std::mutex shutdown_mutex;
  std::condition_variable shutdown_cv;
  bool shutdown_signalled = false;
};

bool Server::shutdown_requested() const noexcept {
  return impl_ != nullptr && impl_->service != nullptr && impl_->service->shutdown_requested();
}

void Server::wait_for_shutdown() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(impl_->shutdown_mutex);
  impl_->shutdown_cv.wait(lock, [this] { return impl_->shutdown_signalled; });
}

Value ServerStats::to_value() const {
  return Value::object({
      {"bytes_read", Value::uint_value(bytes_read)},
      {"bytes_written", Value::uint_value(bytes_written)},
      {"connections_accepted", Value::uint_value(connections_accepted)},
      {"connections_completed", Value::uint_value(connections_completed)},
      {"connections_refused", Value::uint_value(connections_refused)},
      {"frames_rejected", Value::uint_value(frames_rejected)},
      {"requests_refused", Value::uint_value(requests_refused)},
      {"requests_served", Value::uint_value(requests_served)},
  });
}

ServerStats Server::stats() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->stats_mutex);
  return impl_->stats;
}

Result<std::unique_ptr<Server>> Server::start(Fabric& fabric, Options options) noexcept {
  if (options.worker_threads == 0U || options.max_connections == 0U) {
    return Status(ReasonCode::RefusedInvalidRange, "worker and connection bounds must be greater than zero");
  }
  if (options.max_frame_bytes == 0U || options.max_frame_bytes > kMaxBufferedBytes) {
    return Status(ReasonCode::RefusedInvalidRange, "frame bound is outside the supported range");
  }
  std::uint16_t bound_port = 0;
  auto socket = listen_on(options.bind_address, options.port, bound_port);
  SNCF_TRY(socket);

  auto server = std::unique_ptr<Server>(new Server());
  server->impl_ = std::make_unique<Impl>();
  server->impl_->fabric = &fabric;
  server->impl_->options = std::move(options);
  server->impl_->listen_socket = socket.value();
  server->port_ = bound_port;
  server->impl_->service = std::make_unique<FabricService>(fabric);
  server->impl_->accept_thread = std::thread([raw = server.get()] { raw->accept_main(); });
  for (std::size_t i = 0; i < server->impl_->options.worker_threads; ++i) {
    server->impl_->workers.emplace_back([raw = server.get()] { raw->worker_main(); });
  }
  return server;
}

Server::~Server() { stop(); }

void Server::accept_main() noexcept {
  Impl& impl = *impl_;
  for (;;) {
    sockaddr_in remote{};
#if defined(_WIN32)
    int length = sizeof(remote);
#else
    socklen_t length = sizeof(remote);
#endif
    const socket_t client =
        ::accept(impl.listen_socket, reinterpret_cast<sockaddr*>(&remote), &length);
    if (client == kInvalidSocket) {
      return;
    }
    if (impl.stopping.load(std::memory_order_acquire)) {
      close_socket(client);
      return;
    }
    bool refused = false;
    {
      std::lock_guard<std::mutex> guard(impl.mutex);
      const std::size_t live = impl.queue.size() + impl.active.size();
      if (live >= impl.options.max_connections) {
        refused = true;
      } else {
        impl.queue.push_back(client);
      }
    }
    // Statistics are updated under their own lock only, so the reader in
    // stats() never observes a torn counter.
    {
      std::lock_guard<std::mutex> guard(impl.stats_mutex);
      if (refused) {
        ++impl.stats.connections_refused;
      } else {
        ++impl.stats.connections_accepted;
      }
    }
    if (refused) {
      const Value document = protocol_error_value(
          Status(ReasonCode::RefusedOverloaded, "the server connection bound is saturated"));
      Frame frame;
      frame.kind = FrameKind::Error;
      frame.payload = document.to_canonical();
      const std::string bytes = frame.encode();
      (void)send_all(client, bytes);
      shutdown_socket(client);
      close_socket(client);
      continue;
    }
    impl.cv.notify_one();
  }
}

void Server::worker_main() noexcept {
  Impl& impl = *impl_;
  for (;;) {
    socket_t client = kInvalidSocket;
    {
      std::unique_lock<std::mutex> lock(impl.mutex);
      impl.cv.wait(lock, [&impl] { return !impl.queue.empty() || impl.accept_closed; });
      if (impl.queue.empty()) {
        return;
      }
      client = impl.queue.front();
      impl.queue.pop_front();
      impl.active.insert(client);
      if (impl.broadcast_done) {
        shutdown_socket(client);
      }
    }
    if (client != kInvalidSocket) {
      serve_connection(static_cast<std::intptr_t>(client));
    }
    {
      std::lock_guard<std::mutex> guard(impl.mutex);
      impl.active.erase(client);
    }
    {
      std::lock_guard<std::mutex> guard(impl.stats_mutex);
      ++impl.stats.connections_completed;
    }
    shutdown_socket(client);
    close_socket(client);
  }
}

void Server::serve_connection(std::intptr_t client_handle) noexcept {
  Impl& impl = *impl_;
  const socket_t client = static_cast<socket_t>(client_handle);
  std::string buffer;
  std::vector<char> chunk(kReadChunk);
  for (;;) {
    std::size_t consumed = 0;
    auto decoded = decode_frame(buffer, impl.options.max_frame_bytes, consumed);
    if (decoded.ok()) {
      buffer.erase(0, consumed);
      Frame reply;
      reply.request_id = decoded.value().request_id;
      if (decoded.value().kind == FrameKind::Goodbye) {
        return;
      }
      if (decoded.value().kind != FrameKind::Request) {
        std::lock_guard<std::mutex> guard(impl.stats_mutex);
        ++impl.stats.frames_rejected;
        reply.kind = FrameKind::Error;
        reply.payload = protocol_error_value(
                            Status(ReasonCode::RefusedMalformedInput, "only request frames are accepted"))
                            .to_canonical();
        const std::string bytes = reply.encode();
        (void)send_all(client, bytes);
        return;
      }
      auto parsed = parse_canonical(decoded.value().payload, ParseLimits{});
      if (!parsed.ok()) {
        std::lock_guard<std::mutex> guard(impl.stats_mutex);
        ++impl.stats.frames_rejected;
        reply.kind = FrameKind::Error;
        reply.payload = protocol_error_value(parsed.status()).to_canonical();
        const std::string bytes = reply.encode();
        (void)send_all(client, bytes);
        return;
      }
      const Value* op = parsed.value().find("op");
      const Value* args = parsed.value().find("args");
      Result<Value> result =
          Status(ReasonCode::RefusedMissingField, "request must carry exactly an op and args");
      if (parsed.value().as_object().size() != 2U) {
        result = Status(ReasonCode::RefusedUnknownField, "the request document carries unknown fields");
      } else if (op == nullptr || !op->is_string()) {
        result = Status(ReasonCode::RefusedMissingField, "request must name an operation as a string");
      } else if (args == nullptr || !args->is_object()) {
        result = Status(ReasonCode::RefusedMissingField, "request must carry an arguments object");
      } else {
        result = impl.service->dispatch(op->as_string(), *args);
      }
      if (result.ok()) {
        reply.kind = FrameKind::Response;
        reply.payload = result.value().to_canonical();
        std::lock_guard<std::mutex> guard(impl.stats_mutex);
        ++impl.stats.requests_served;
      } else {
        reply.kind = FrameKind::Error;
        reply.payload = protocol_error_value(result.status()).to_canonical();
        std::lock_guard<std::mutex> guard(impl.stats_mutex);
        ++impl.stats.requests_refused;
      }
      const std::string bytes = reply.encode();
      auto sent = send_all(client, bytes);
      {
        std::lock_guard<std::mutex> guard(impl.stats_mutex);
        if (sent.ok()) {
          impl.stats.bytes_written += sent.value();
        }
      }
      if (!sent.ok()) {
        return;
      }
      if (impl.service->shutdown_requested()) {
        {
          std::lock_guard<std::mutex> guard(impl.shutdown_mutex);
          impl.shutdown_signalled = true;
        }
        impl.shutdown_cv.notify_all();
        return;
      }
      continue;
    }
    if (decoded.code() == ReasonCode::RefusedTruncatedInput) {
      if (buffer.size() > kMaxBufferedBytes) {
        std::lock_guard<std::mutex> guard(impl.stats_mutex);
        ++impl.stats.frames_rejected;
        return;
      }
    } else {
      // A frame that is malformed cannot be resynchronised: refuse and close.
      Frame reply;
      reply.kind = FrameKind::Error;
      reply.payload = protocol_error_value(decoded.status()).to_canonical();
      const std::string bytes = reply.encode();
      (void)send_all(client, bytes);
      std::lock_guard<std::mutex> guard(impl.stats_mutex);
      ++impl.stats.frames_rejected;
      return;
    }
    const auto received = recv_some(client, chunk.data(), chunk.size());
    if (!received.ok()) {
      return;
    }
    if (received.value() == 0U) {
      return;
    }
    buffer.append(chunk.data(), received.value());
    {
      std::lock_guard<std::mutex> guard(impl.stats_mutex);
      impl.stats.bytes_read += received.value();
    }
  }
}

void Server::stop() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  Impl& impl = *impl_;
  // Serialise teardown: a second caller waits until the first has finished, so
  // "stop returned" always means the server is fully torn down.
  std::lock_guard<std::mutex> teardown_guard(impl.teardown_mutex);
  if (impl.stopping.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  // Wake the accept loop without waiting on a timeout.
  auto waker = connect_to(impl.options.bind_address, port_);
  if (waker.ok()) {
    close_socket(waker.value());
  }
  if (impl.accept_thread.joinable() && impl.accept_thread.get_id() != std::this_thread::get_id()) {
    impl.accept_thread.join();
  }
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    impl.accept_closed = true;
    impl.broadcast_done = true;
    for (const auto queued : impl.queue) {
      shutdown_socket(queued);
      close_socket(queued);
    }
    impl.queue.clear();
    for (const auto live : impl.active) {
      shutdown_socket(live);
    }
  }
  impl.cv.notify_all();
  {
    // Release any waiter blocked in wait_for_shutdown so stop() can join.
    std::lock_guard<std::mutex> guard(impl.shutdown_mutex);
    impl.shutdown_signalled = true;
  }
  impl.shutdown_cv.notify_all();
  for (auto& worker : impl.workers) {
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      worker.join();
    }
  }
  impl.workers.clear();
  close_socket(impl.listen_socket);
  impl.listen_socket = kInvalidSocket;
  // The service outlives stop(): it is released with the server itself, so a
  // concurrent shutdown_requested() never dereferences a reset pointer.
}

Client::~Client() { close(); }

Result<std::unique_ptr<Client>> Client::connect(std::string host, std::uint16_t port) noexcept {
  auto socket = connect_to(host, port);
  SNCF_TRY(socket);
  auto client = std::unique_ptr<Client>(new Client());
  client->socket_ = static_cast<std::intptr_t>(socket.value());
  return client;
}

void Client::close() noexcept {
  if (socket_ != -1) {
    shutdown_socket(static_cast<socket_t>(socket_));
    close_socket(static_cast<socket_t>(socket_));
    socket_ = -1;
  }
}

Result<Value> Client::call(std::string_view operation, Value arguments) noexcept {
  Value request = Value::object({
      {"args", std::move(arguments)},
      {"op", Value::string(std::string(operation))},
  });
  Frame frame;
  frame.kind = FrameKind::Request;
  frame.request_id = next_request_id_++;
  frame.payload = request.to_canonical();
  const std::string bytes = frame.encode();
  auto sent = send_all(static_cast<socket_t>(socket_), bytes);
  SNCF_TRY(sent);

  std::vector<char> chunk(kReadChunk);
  for (;;) {
    std::size_t consumed = 0;
    auto decoded = decode_frame(buffer_, kMaxBufferedBytes, consumed);
    if (decoded.ok()) {
      buffer_.erase(0, consumed);
      // An error raised before the request could be parsed carries no request
      // identity; a response always must match it.
      if (decoded.value().kind != FrameKind::Error &&
          decoded.value().request_id != frame.request_id) {
        return Status(ReasonCode::RefusedMalformedInput, "response does not match the request identity");
      }
      auto parsed = parse_canonical(decoded.value().payload, ParseLimits{});
      SNCF_TRY(parsed);
      if (decoded.value().kind == FrameKind::Error) {
        return protocol_error_status(parsed.value());
      }
      if (decoded.value().kind != FrameKind::Response) {
        return Status(ReasonCode::RefusedMalformedInput, "unexpected frame kind in response");
      }
      return parsed.value();
    }
    if (decoded.code() != ReasonCode::RefusedTruncatedInput) {
      return decoded.status();
    }
    if (buffer_.size() > kMaxBufferedBytes) {
      return Status(ReasonCode::RefusedOversizedInput, "response exceeds the buffered bound");
    }
    auto received = recv_some(static_cast<socket_t>(socket_), chunk.data(), chunk.size());
    SNCF_TRY(received);
    if (received.value() == 0U) {
      return Status(ReasonCode::RefusedTruncatedInput, "the peer closed before a complete frame arrived");
    }
    buffer_.append(chunk.data(), received.value());
  }
}

Result<Value> Client::send_raw_frame(std::string_view bytes) noexcept {
  auto sent = send_all(static_cast<socket_t>(socket_), bytes);
  SNCF_TRY(sent);
  std::vector<char> chunk(kReadChunk);
  for (;;) {
    std::size_t consumed = 0;
    auto decoded = decode_frame(buffer_, kMaxBufferedBytes, consumed);
    if (decoded.ok()) {
      buffer_.erase(0, consumed);
      auto parsed = parse_canonical(decoded.value().payload, ParseLimits{});
      SNCF_TRY(parsed);
      if (decoded.value().kind == FrameKind::Error) {
        return protocol_error_status(parsed.value());
      }
      return parsed.value();
    }
    if (decoded.code() != ReasonCode::RefusedTruncatedInput) {
      return decoded.status();
    }
    if (buffer_.size() > kMaxBufferedBytes) {
      return Status(ReasonCode::RefusedOversizedInput, "response exceeds the buffered bound");
    }
    auto received = recv_some(static_cast<socket_t>(socket_), chunk.data(), chunk.size());
    SNCF_TRY(received);
    if (received.value() == 0U) {
      return Status(ReasonCode::RefusedTruncatedInput, "the peer closed before a complete frame arrived");
    }
    buffer_.append(chunk.data(), received.value());
  }
}

Result<void> Client::send_bytes(std::string_view bytes) noexcept {
  auto sent = send_all(static_cast<socket_t>(socket_), bytes);
  SNCF_TRY(sent);
  return {};
}

void Client::shutdown_send() noexcept {
  if (socket_ == -1) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(static_cast<socket_t>(socket_), SD_SEND);
#else
  ::shutdown(static_cast<socket_t>(socket_), SHUT_WR);
#endif
}

Result<Frame> Client::read_frame() noexcept {
  std::vector<char> chunk(kReadChunk);
  for (;;) {
    std::size_t consumed = 0;
    auto decoded = decode_frame(buffer_, kMaxBufferedBytes, consumed);
    if (decoded.ok()) {
      buffer_.erase(0, consumed);
      return decoded.value();
    }
    if (decoded.code() != ReasonCode::RefusedTruncatedInput) {
      return decoded.status();
    }
    if (buffer_.size() > kMaxBufferedBytes) {
      return Status(ReasonCode::RefusedOversizedInput, "response exceeds the buffered bound");
    }
    auto received = recv_some(static_cast<socket_t>(socket_), chunk.data(), chunk.size());
    SNCF_TRY(received);
    if (received.value() == 0U) {
      return Status(ReasonCode::RefusedTruncatedInput, "the peer closed before a complete frame arrived");
    }
    buffer_.append(chunk.data(), received.value());
  }
}

Result<Value> Client::call_raw_payload(std::string_view payload) noexcept {
  Frame frame;
  frame.kind = FrameKind::Request;
  frame.request_id = next_request_id_++;
  frame.payload.assign(payload);
  return send_raw_frame(frame.encode());
}

}  // namespace sncf
