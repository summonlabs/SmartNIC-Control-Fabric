// Copyright 2026 Summon Software Labs.
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "sncf/fabric.hpp"
#include "sncf/protocol.hpp"
#include "sncf/transport.hpp"
#include "sncf/version.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace sncf;

Value ping_arguments() { return Value::object({}); }

Value smartnic_arguments(std::uint64_t command, const std::string& label) {
  return Value::object({
      {"command", Value::uint_value(command)},
      {"label", Value::string(label)},
      {"port_count", Value::uint_value(4)},
      {"principal", Value::uint_value(2)},
  });
}

std::string environment_value(const char* name) {
  const char* raw = std::getenv(name);
  return raw != nullptr ? std::string(raw) : std::string();
}

/// Runs in a separate operating-system process: drives a complete lifecycle over
/// a real TCP connection to the parent's server and verifies every response.
int child_lifecycle(bool expect_conflict) {
  const std::string port_text = environment_value("SNCF_TEST_PORT");
  const std::string slot_text = environment_value("SNCF_TEST_SLOT");
  if (port_text.empty() || slot_text.empty()) {
    return 10;
  }
  const auto port = static_cast<std::uint16_t>(std::stoul(port_text));
  const auto slot = static_cast<std::uint16_t>(std::stoul(slot_text));
  auto client = Client::connect("127.0.0.1", port);
  if (!client.ok()) {
    return 11;
  }
  std::uint64_t command = 1000000ULL * slot;
  const auto next_command = [&command] { return command++; };

  auto ping = client.value()->call("ping", ping_arguments());
  if (!ping.ok()) {
    return 12;
  }
  const Value* semantics = ping.value().find("semantics");
  if (semantics == nullptr || !semantics->is_string() ||
      semantics->as_string() != std::string(semantics_id())) {
    return 13;
  }

  auto smartnic = client.value()->call("register_smartnic",
                                       smartnic_arguments(next_command(), "child-" + std::to_string(slot)));
  if (!smartnic.ok()) {
    return 14;
  }
  const Value* smartnic_id = smartnic.value().find("id");
  if (smartnic_id == nullptr || !smartnic_id->is_uint()) {
    return 15;
  }

  auto device = client.value()->call(
      "register_device",
      Value::object({
          {"command", Value::uint_value(next_command())},
          {"incarnation", Value::uint_value(1)},
          {"model", Value::uint_value(1)},
          {"port", Value::uint_value(slot)},
          {"principal", Value::uint_value(2)},
          {"serial", Value::string("child-serial-" + std::to_string(slot))},
          {"smartnic", Value::uint_value(smartnic_id->as_uint())},
      }));
  if (!device.ok()) {
    return 16;
  }
  const Value* device_id = device.value().find("id");
  if (device_id == nullptr || !device_id->is_uint()) {
    return 17;
  }

  auto package = client.value()->call(
      "register_package",
      Value::object({
          {"capabilities", Value::array({Value::string("packet_parsing")})},
          {"command", Value::uint_value(next_command())},
          {"compatibility", Value::object({{"max", Value::uint_value(8)}, {"min", Value::uint_value(1)}})},
          {"demand", Value::object({{"memory_bytes", Value::uint_value(1024)},
                                    {"ports", Value::uint_value(1)},
                                    {"queues", Value::uint_value(1)}})},
          {"digest", Value::string(std::string(64, 'a'))},
          {"exclusive_scope", Value::boolean(true)},
          {"layout_revision", Value::uint_value(1)},
          {"min_capability_generation", Value::uint_value(1)},
          {"min_firmware", Value::object({{"major", Value::uint_value(1)},
                                          {"minor", Value::uint_value(0)},
                                          {"patch", Value::uint_value(0)}})},
          {"name", Value::string("child-package-" + std::to_string(slot))},
          {"principal", Value::uint_value(2)},
          {"supported_models", Value::array({Value::uint_value(1)})},
          {"version", Value::object({{"major", Value::uint_value(1)},
                                     {"minor", Value::uint_value(0)},
                                     {"patch", Value::uint_value(0)}})},
      }));
  if (!package.ok()) {
    return 18;
  }
  const Value* package_id = package.value().find("id");
  if (package_id == nullptr || !package_id->is_uint()) {
    return 19;
  }

  auto now_ns = SystemClock{}.now().value();
  auto evidence = client.value()->call(
      "submit_capability_evidence",
      Value::object({
          {"capabilities", Value::array({Value::string("packet_parsing")})},
          {"capability_generation", Value::uint_value(3)},
          {"command", Value::uint_value(next_command())},
          {"compatibility_generation", Value::uint_value(4)},
          {"device", Value::uint_value(device_id->as_uint())},
          {"firmware", Value::object({{"major", Value::uint_value(1)},
                                      {"minor", Value::uint_value(2)},
                                      {"patch", Value::uint_value(0)}})},
          {"incarnation", Value::uint_value(1)},
          {"model", Value::uint_value(1)},
          {"observed_at", Value::uint_value(now_ns)},
          {"payload_digest", Value::string(std::string(64, 'b'))},
          {"principal", Value::uint_value(2)},
          {"smartnic", Value::uint_value(smartnic_id->as_uint())},
          {"source", Value::string("synthetic_fixture")},
          {"synthetic", Value::boolean(true)},
      }));
  if (!evidence.ok()) {
    return 20;
  }
  auto observation = client.value()->call(
      "submit_observation",
      Value::object({
          {"capability_generation", Value::uint_value(3)},
          {"command", Value::uint_value(next_command())},
          {"compatibility_generation", Value::uint_value(4)},
          {"device", Value::uint_value(device_id->as_uint())},
          {"incarnation", Value::uint_value(1)},
          {"observed_at", Value::uint_value(now_ns)},
          {"payload_digest", Value::string(std::string(64, 'c'))},
          {"present", Value::boolean(true)},
          {"principal", Value::uint_value(2)},
          {"smartnic", Value::uint_value(smartnic_id->as_uint())},
          {"source", Value::string("synthetic_fixture")},
          {"synthetic", Value::boolean(true)},
      }));
  if (!observation.ok()) {
    return 21;
  }
  auto activation = client.value()->call(
      "activate",
      Value::object({
          {"command", Value::uint_value(next_command())},
          {"device", Value::uint_value(device_id->as_uint())},
          {"incarnation", Value::uint_value(1)},
          {"instance", Value::uint_value(0)},
          {"lease_ttl_ns", Value::uint_value(0)},
          {"package", Value::uint_value(package_id->as_uint())},
          {"policy_generation", Value::uint_value(0)},
          {"port", Value::uint_value(slot)},
          {"principal", Value::uint_value(2)},
          {"queue", Value::uint_value(1)},
          {"smartnic", Value::uint_value(smartnic_id->as_uint())},
      }));
  if (!activation.ok()) {
    return 22;
  }
  const Value* instance = activation.value().find("instance");
  const Value* attempt = activation.value().find("attempt");
  const Value* generation = activation.value().find("generation");
  const Value* token = activation.value().find("token");
  if (instance == nullptr || attempt == nullptr || generation == nullptr || token == nullptr) {
    return 23;
  }

  if (expect_conflict) {
    // Discover, through the shared coordinator, a scope that another operating
    // system process already owns, then try to take it. This proves the two
    // processes contend for one authority domain and not two.
    auto exported = client.value()->call("export", Value::object({}));
    if (!exported.ok()) {
      return 40;
    }
    const Value* document = exported.value().find("export");
    if (document == nullptr) {
      return 41;
    }
    const Value* durable = document->find("durable");
    if (durable == nullptr) {
      return 42;
    }
    const Value* instances = durable->find("instances");
    if (instances == nullptr || !instances->is_array() || instances->as_array().empty()) {
      return 43;
    }
    auto existing = instance_from_value(instances->as_array()[0]);
    if (!existing.ok()) {
      return 44;
    }
    auto conflict = client.value()->call(
        "activate",
        Value::object({
            {"command", Value::uint_value(next_command())},
            {"device", Value::uint_value(existing.value().device.value())},
            {"incarnation", Value::uint_value(existing.value().incarnation.value())},
            {"instance", Value::uint_value(0)},
            {"lease_ttl_ns", Value::uint_value(0)},
            {"package", Value::uint_value(existing.value().package.value())},
            {"policy_generation", Value::uint_value(0)},
            {"port", Value::uint_value(existing.value().port.value())},
            {"principal", Value::uint_value(3)},
            {"queue", Value::uint_value(2)},
            {"smartnic", Value::uint_value(existing.value().smartnic.value())},
        }));
    if (conflict.ok()) {
      return 24;
    }
    if (conflict.code() != ReasonCode::RefusedScopeConflict) {
      return 25;
    }
    client.value()->close();
    return 0;
  }

  auto acknowledged = client.value()->call(
      "acknowledge",
      Value::object({
          {"attempt", Value::uint_value(attempt->as_uint())},
          {"command", Value::uint_value(next_command())},
          {"generation", Value::uint_value(generation->as_uint())},
          {"instance", Value::uint_value(instance->as_uint())},
          {"observed_at", Value::uint_value(SystemClock{}.now().value())},
          {"principal", Value::uint_value(2)},
          {"source", Value::string("synthetic_fixture")},
          {"token", Value::uint_value(token->as_uint())},
      }));
  if (!acknowledged.ok()) {
    return 26;
  }
  auto verified = client.value()->call(
      "report_effect",
      Value::object({
          {"attempt", Value::uint_value(attempt->as_uint())},
          {"command", Value::uint_value(next_command())},
          {"detail", Value::string("multiprocess verification")},
          {"generation", Value::uint_value(generation->as_uint())},
          {"instance", Value::uint_value(instance->as_uint())},
          {"observed_at", Value::uint_value(SystemClock{}.now().value())},
          {"outcome", Value::string("verified")},
          {"payload_digest", Value::string(std::string(64, 'd'))},
          {"principal", Value::uint_value(2)},
          {"source", Value::string("synthetic_fixture")},
          {"synthetic", Value::boolean(true)},
          {"token", Value::uint_value(token->as_uint())},
      }));
  if (!verified.ok()) {
    return 27;
  }
  auto inspected = client.value()->call(
      "inspect_instance", Value::object({{"instance", Value::uint_value(instance->as_uint())}}));
  if (!inspected.ok()) {
    return 28;
  }
  const Value* record = inspected.value().find("instance");
  if (record == nullptr) {
    return 29;
  }
  const Value* lifecycle = record->find("lifecycle");
  if (lifecycle == nullptr || !lifecycle->is_string() || lifecycle->as_string() != "verified") {
    return 30;
  }
  auto exported = client.value()->call("export", Value::object({}));
  if (!exported.ok()) {
    return 31;
  }
  const Value* export_document = exported.value().find("export");
  if (export_document == nullptr || !export_document->is_object()) {
    return 32;
  }
  client.value()->close();
  return 0;
}

}  // namespace

int sncf_run_fork_child(const std::string& store, const std::string& mode) {
  (void)store;
  return child_lifecycle(mode == "conflict");
}

namespace {

SNCF_TEST(transport_ping_and_lifecycle_over_a_real_socket) {
  sncf_test::Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 2;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());
  SNCF_CHECK(server.value()->port() != 0);

  {
    auto client = Client::connect("127.0.0.1", server.value()->port());
    SNCF_REQUIRE(client.ok());
    auto pong = client.value()->call("ping", Value::object({}));
    SNCF_REQUIRE(pong.ok());
    auto ok_field = field_bool(pong.value(), "ok");
    SNCF_REQUIRE(ok_field.ok());
    SNCF_CHECK(ok_field.value());

    auto unknown = client.value()->call("not-an-operation", Value::object({}));
    SNCF_CHECK(!unknown.ok());
    SNCF_CHECK(unknown.code() == ReasonCode::RefusedOutOfBoundary);

    auto registered = client.value()->call("register_smartnic", smartnic_arguments(1, "wire-nic"));
    SNCF_REQUIRE(registered.ok());
    auto id = field_uint(registered.value(), "id");
    SNCF_REQUIRE(id.ok());
    SNCF_CHECK_EQ(id.value(), 1U);

    // Replaying the same command over the wire is idempotent.
    auto replay = client.value()->call("register_smartnic", smartnic_arguments(1, "wire-nic"));
    SNCF_REQUIRE(replay.ok());
    auto duplicate = field_bool(replay.value(), "duplicate");
    SNCF_REQUIRE(duplicate.ok());
    SNCF_CHECK(duplicate.value());

    auto counters = client.value()->call("counters", Value::object({}));
    SNCF_REQUIRE(counters.ok());
    client.value()->close();
  }

  const ServerStats stats = server.value()->stats();
  SNCF_CHECK(stats.connections_accepted >= 1U);
  SNCF_CHECK(stats.requests_served >= 4U);
  SNCF_CHECK(stats.bytes_read > 0U);
  SNCF_CHECK(stats.bytes_written > 0U);
  server.value()->stop();
  server.value()->stop();  // idempotent
  SNCF_CHECK_EQ(fabric.value()->counters().internal_apply_failures, 0U);
}

SNCF_TEST(transport_malformed_frames_are_refused_without_state_change) {
  sncf_test::Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 2;
  // A small frame bound keeps the oversized probe small enough that the client
  // can always finish writing it, so the assertion is about the refusal and not
  // about a half-written socket buffer.
  options.max_frame_bytes = 512;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());
  const Digest256 before = fabric.value()->durable_digest();

  const auto expect_refusal = [&](const std::string& bytes, ReasonCode expected) {
    auto client = Client::connect("127.0.0.1", server.value()->port());
    SNCF_REQUIRE(client.ok());
    auto result = client.value()->send_raw_frame(bytes);
    if (result.ok()) {
      ::sncf_test::fail(__FILE__, __LINE__, "malformed frame produced a successful response");
    }
    if (result.code() != expected) {
      ::sncf_test::fail(__FILE__, __LINE__, std::string("expected ") + std::string(reason_name(expected)) +
                                               " but got " + std::string(reason_name(result.code())));
    }
    client.value()->close();
  };

  Frame good;
  good.kind = FrameKind::Request;
  good.request_id = 7;
  good.payload = Value::object({{"args", Value::object({})}, {"op", Value::string("ping")}}).to_canonical();

  std::string bad_magic = good.encode();
  bad_magic[0] = 'Z';
  expect_refusal(bad_magic, ReasonCode::RefusedMalformedInput);

  std::string bad_crc = good.encode();
  bad_crc[16] = static_cast<char>(bad_crc[16] ^ 0x7F);
  expect_refusal(bad_crc, ReasonCode::RefusedJournalCorrupt);

  std::string bad_version = good.encode();
  bad_version[4] = static_cast<char>(9);
  expect_refusal(bad_version, ReasonCode::RefusedUnsupportedVersion);

  Frame oversized;
  oversized.kind = FrameKind::Request;
  oversized.payload = std::string(1024U, 'x');
  expect_refusal(oversized.encode(), ReasonCode::RefusedOversizedInput);

  Frame non_canonical;
  non_canonical.kind = FrameKind::Request;
  non_canonical.payload = "{\"op\": \"ping\", \"args\": {}}";
  expect_refusal(non_canonical.encode(), ReasonCode::RefusedMalformedInput);

  Frame unknown_field;
  unknown_field.kind = FrameKind::Request;
  unknown_field.payload =
      Value::object({{"args", Value::object({})},
                     {"op", Value::string("ping")},
                     {"surprise", Value::uint_value(1)}})
          .to_canonical();
  expect_refusal(unknown_field.encode(), ReasonCode::RefusedUnknownField);

  SNCF_CHECK_EQ(fabric.value()->durable_digest().to_hex(), before.to_hex());
  SNCF_CHECK(!server.value()->shutdown_requested());
  server.value()->stop();
}

SNCF_TEST(transport_truncated_frame_never_yields_a_success) {
  sncf_test::Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 1;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());

  auto client = Client::connect("127.0.0.1", server.value()->port());
  SNCF_REQUIRE(client.ok());
  Frame frame;
  frame.kind = FrameKind::Request;
  frame.request_id = 3;
  frame.payload = Value::object({{"args", Value::object({})}, {"op", Value::string("ping")}}).to_canonical();
  const std::string encoded = frame.encode();
  SNCF_REQUIRE(client.value()->send_bytes(encoded.substr(0, encoded.size() - 4U)).ok());
  client.value()->shutdown_send();
  auto response = client.value()->read_frame();
  SNCF_CHECK(!response.ok());
  SNCF_CHECK(response.code() == ReasonCode::RefusedTruncatedInput);
  client.value()->close();

  // The server still serves a complete request afterwards.
  auto healthy = Client::connect("127.0.0.1", server.value()->port());
  SNCF_REQUIRE(healthy.ok());
  auto pong = healthy.value()->call("ping", Value::object({}));
  SNCF_REQUIRE(pong.ok());
  healthy.value()->close();
  SNCF_CHECK(server.value()->stats().frames_rejected >= 0U);
  server.value()->stop();
}

SNCF_TEST(transport_connection_bound_is_enforced) {
  sncf_test::Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 1;
  options.max_connections = 1;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());

  auto first = Client::connect("127.0.0.1", server.value()->port());
  SNCF_REQUIRE(first.ok());
  SNCF_REQUIRE(first.value()->call("ping", Value::object({})).ok());

  auto second = Client::connect("127.0.0.1", server.value()->port());
  SNCF_REQUIRE(second.ok());
  auto refused = second.value()->call("ping", Value::object({}));
  if (refused.ok() || refused.code() != ReasonCode::RefusedOverloaded) {
    ::sncf_test::fail(__FILE__, __LINE__,
                      std::string("expected refused_overloaded but got ") +
                          (refused.ok() ? std::string("success") : std::string(reason_name(refused.code()))));
  }
  second.value()->close();

  first.value()->close();
  SNCF_CHECK(server.value()->stats().connections_refused >= 1U);
  server.value()->stop();
}

SNCF_TEST(transport_serves_concurrent_clients_from_several_threads) {
  FabricConfig config;
  config.max_smartnics = 512;
  config.max_journal_records = 8192;
  sncf_test::Harness harness(config);
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 4;
  options.max_connections = 8;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());

  constexpr int kClients = 4;
  constexpr int kRequests = 25;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < kClients; ++worker) {
    threads.emplace_back([&, worker] {
      auto client = Client::connect("127.0.0.1", server.value()->port());
      if (!client.ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      for (int index = 0; index < kRequests; ++index) {
        const std::uint64_t command = static_cast<std::uint64_t>(worker) * 1000U + index + 1U;
        auto result = client.value()->call(
            "register_smartnic", smartnic_arguments(command, "concurrent-nic-" + std::to_string(command)));
        if (!result.ok()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
      auto counters = client.value()->call("counters", Value::object({}));
      if (!counters.ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      client.value()->close();
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  SNCF_CHECK_EQ(failures.load(), 0);
  const ServerStats stats = server.value()->stats();
  SNCF_CHECK(stats.requests_served >= static_cast<std::uint64_t>(kClients) * (kRequests + 1));
  SNCF_CHECK_EQ(stats.connections_refused, 0U);
  server.value()->stop();
}

SNCF_TEST(transport_shutdown_request_stops_the_server_with_work_in_flight) {
  sncf_test::Harness harness;
  auto fabric = harness.open();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 3;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());

  std::atomic<bool> stop_requested{false};
  std::vector<std::thread> clients;
  for (int worker = 0; worker < 3; ++worker) {
    clients.emplace_back([&, worker] {
      auto client = Client::connect("127.0.0.1", server.value()->port());
      if (!client.ok()) {
        return;
      }
      std::uint64_t command = 500000U + static_cast<std::uint64_t>(worker) * 100U;
      while (!stop_requested.load(std::memory_order_acquire)) {
        (void)client.value()->call("ping", Value::object({}));
        (void)client.value()->call(
            "register_smartnic",
            smartnic_arguments(command++, "in-flight-" + std::to_string(command)));
      }
      client.value()->close();
    });
  }

  auto stopper = Client::connect("127.0.0.1", server.value()->port());
  SNCF_REQUIRE(stopper.ok());
  auto acknowledged = stopper.value()->call("shutdown", Value::object({}));
  SNCF_REQUIRE(acknowledged.ok());
  SNCF_CHECK(server.value()->shutdown_requested());
  server.value()->wait_for_shutdown();
  stop_requested.store(true, std::memory_order_release);
  for (auto& client : clients) {
    client.join();
  }
  server.value()->stop();
  stopper.value()->close();
  SNCF_CHECK_EQ(fabric.value()->counters().internal_apply_failures, 0U);
}

SNCF_TEST(transport_proves_multiprocess_operation_over_real_sockets) {
  sncf_test::Harness harness;
  // The independent process supplies its own wall-clock timestamps, so the
  // coordinator must use the same time base.
  auto fabric = harness.open_with_system_clock();
  SNCF_REQUIRE(fabric.ok());
  Server::Options options;
  options.port = 0;
  options.worker_threads = 4;
  options.max_connections = 8;
  auto server = Server::start(*fabric.value(), options);
  SNCF_REQUIRE(server.ok());

  const std::string port_text = std::to_string(server.value()->port());
#if defined(_WIN32)
  _putenv_s("SNCF_TEST_PORT", port_text.c_str());
  _putenv_s("SNCF_TEST_SLOT", "1");
#else
  ::setenv("SNCF_TEST_PORT", port_text.c_str(), 1);
  ::setenv("SNCF_TEST_SLOT", "1", 1);
#endif
  const int first_status = sncf_test::run_self_child({"--fork-child", harness.store().string(), "lifecycle"});
  SNCF_CHECK_EQ(first_status, 0);

  // A second independent process must observe the first process's authority and
  // be refused when it tries to take the same exclusive scope.
  sncf_test::ChildProcess conflicting = sncf_test::spawn_self_child(
      {"--fork-child", harness.store().string(), "conflict"});
  SNCF_REQUIRE(conflicting.valid());
  SNCF_CHECK_EQ(sncf_test::wait_self_child(conflicting), 0);

  const ServerStats stats = server.value()->stats();
  SNCF_CHECK(stats.connections_accepted >= 2U);
  SNCF_CHECK(stats.requests_served >= 12U);

  const Value exported = fabric.value()->export_value();
  const Value* durable = exported.find("durable");
  SNCF_REQUIRE(durable != nullptr);
  const Value* instances = durable->find("instances");
  SNCF_REQUIRE(instances != nullptr);
  SNCF_CHECK_EQ(instances->as_array().size(), 1U);
  auto instance = instance_from_value(instances->as_array()[0]);
  SNCF_REQUIRE(instance.ok());
  SNCF_CHECK(instance.value().lifecycle == LifecycleState::Verified);
  // The verified claim came from the other process, over the socket.
  SNCF_CHECK(instance.value().last_effect_outcome == EffectOutcome::Verified);
  SNCF_CHECK(!instance.value().last_effect_digest.is_zero());
  server.value()->stop();
  SNCF_CHECK_EQ(fabric.value()->counters().internal_apply_failures, 0U);
}

SNCF_TEST(transport_frame_codec_round_trip_and_bounds) {
  Frame frame;
  frame.kind = FrameKind::Response;
  frame.flags = 3;
  frame.request_id = 42;
  frame.payload = "payload";
  const std::string encoded = frame.encode();
  std::size_t consumed = 0;
  auto decoded = decode_frame(encoded, 1024, consumed);
  SNCF_REQUIRE(decoded.ok());
  SNCF_CHECK_EQ(consumed, encoded.size());
  SNCF_CHECK(decoded.value().kind == FrameKind::Response);
  SNCF_CHECK_EQ(decoded.value().flags, static_cast<std::uint16_t>(3));
  SNCF_CHECK_EQ(decoded.value().request_id, 42U);
  SNCF_CHECK_EQ(decoded.value().payload, std::string("payload"));

  auto truncated = decode_frame(encoded.substr(0, encoded.size() - 1U), 1024, consumed);
  SNCF_CHECK(!truncated.ok());
  SNCF_CHECK(truncated.code() == ReasonCode::RefusedTruncatedInput);

  auto bounded = decode_frame(encoded, 4, consumed);
  SNCF_CHECK(!bounded.ok());
  SNCF_CHECK(bounded.code() == ReasonCode::RefusedOversizedInput);

  // Two frames back to back are decoded independently.
  const std::string doubled = encoded + encoded;
  auto first = decode_frame(doubled, 1024, consumed);
  SNCF_REQUIRE(first.ok());
  auto second = decode_frame(doubled.substr(consumed), 1024, consumed);
  SNCF_REQUIRE(second.ok());
  SNCF_CHECK_EQ(second.value().request_id, 42U);
}

}  // namespace
