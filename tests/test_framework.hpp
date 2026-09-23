// Minimal deterministic test framework. There are no timeouts anywhere: every
// check synchronises on completed work, so a hang is a defect rather than a
// skipped test.
// Copyright 2026 Summon Software Labs.
#ifndef SNCF_TESTS_TEST_FRAMEWORK_HPP
#define SNCF_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <cstdio>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sncf_test {

class AssertionFailure : public std::exception {
 public:
  AssertionFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

using TestFn = void (*)();
using TestBody = void (*)(int);

struct TestCase {
  const char* name;
  const char* file;
  TestFn fn;
};

std::vector<TestCase>& registry();
int register_test(const char* name, const char* file, TestFn fn);

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw AssertionFailure(stream.str());
}

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// Renders a value for a failure message. Types without a stream operator are
/// still usable in comparisons; they simply report as unprintable.
template <class T>
std::string describe(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return std::string("<unprintable>");
  }
}

inline std::string describe(bool value) { return value ? "true" : "false"; }

int run_all(int argc, char** argv);

/// Absolute path of the running test binary, used to spawn real child processes
/// for the crash-boundary and multi-process transport proofs.
void set_self_path(const char* path);
const std::string& self_path();

/// Runs a child process of the test binary and returns its exit code. Blocks
/// until the child terminates; no timeout is applied because the child always
/// terminates by design (either normally or by hard exit at a crash point).
int run_self_child(const std::vector<std::string>& args);

/// Starts a child process of the test binary without waiting for it.
struct ChildProcess {
  std::intptr_t handle = -1;
  bool valid() const noexcept { return handle != -1; }
};

ChildProcess spawn_self_child(const std::vector<std::string>& args);

/// Waits for a child started by spawn_self_child and returns its exit code.
int wait_self_child(ChildProcess child);

}  // namespace sncf_test

#define SNCF_TEST(name)                                                          \
  static void name();                                                            \
  namespace {                                                                    \
  const int sncf_test_registration_##name =                                      \
      ::sncf_test::register_test(#name, __FILE__, &name);                         \
  }                                                                              \
  static void name()

#define SNCF_CHECK(condition)                                                                     \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      ::sncf_test::fail(__FILE__, __LINE__, std::string("check failed: ") + #condition);           \
    }                                                                                             \
  } while (false)

#define SNCF_CHECK_EQ(actual, expected)                                                            \
  do {                                                                                            \
    const auto& sncf_actual = (actual);                                                            \
    const auto& sncf_expected = (expected);                                                        \
    if (!(sncf_actual == sncf_expected)) {                                                         \
      ::sncf_test::fail(__FILE__, __LINE__,                                                        \
                        std::string("expected ") + #actual + " == " + #expected + " but got " +     \
                            ::sncf_test::describe(sncf_actual) + " vs " +                          \
                            ::sncf_test::describe(sncf_expected));                                 \
    }                                                                                             \
  } while (false)

#define SNCF_CHECK_NE(actual, unexpected)                                                          \
  do {                                                                                            \
    if ((actual) == (unexpected)) {                                                                \
      ::sncf_test::fail(__FILE__, __LINE__, std::string("expected ") + #actual + " != " + #unexpected); \
    }                                                                                             \
  } while (false)

#define SNCF_REQUIRE(condition)                                                                    \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      ::sncf_test::fail(__FILE__, __LINE__, std::string("requirement failed: ") + #condition);      \
    }                                                                                             \
  } while (false)

#endif  // SNCF_TESTS_TEST_FRAMEWORK_HPP
