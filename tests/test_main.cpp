// Copyright 2026 Summon Software Labs.
#include "test_framework.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/// Implemented by the restart test unit: performs a scripted workload in a child
/// process and terminates hard at the requested durable boundary.
int sncf_run_crash_writer(const std::string& store, const std::string& point);

namespace sncf_test {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int register_test(const char* name, const char* file, TestFn fn) {
  registry().push_back(TestCase{name, file, fn});
  return 0;
}

namespace {
std::string& self_path_storage() {
  static std::string value;
  return value;
}
}  // namespace

void set_self_path(const char* path) { self_path_storage() = path != nullptr ? path : ""; }

const std::string& self_path() { return self_path_storage(); }

int run_self_child(const std::vector<std::string>& args) {
  std::vector<const char*> argv;
  const std::string program = self_path();
  argv.push_back(program.c_str());
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(nullptr);
#if defined(_WIN32)
  const intptr_t status = _spawnv(_P_WAIT, program.c_str(), argv.data());
  return status < 0 ? -1 : static_cast<int>(status);
#else
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::execv(program.c_str(), const_cast<char* const*>(argv.data()));
    ::_exit(127);
  }
  if (pid < 0) {
    return -1;
  }
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
    }
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  for (const auto& test : registry()) {
    if (filter != nullptr && std::string(test.name).find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    std::printf("[ RUN      ] %s\n", test.name);
    std::fflush(stdout);
    try {
      test.fn();
      std::printf("[       OK ] %s\n", test.name);
      std::fflush(stdout);
      ++passed;
    } catch (const AssertionFailure& failure) {
      std::printf("[  FAILED  ] %s\n            %s\n", test.name, failure.what());
      std::fflush(stdout);
      ++failed;
    } catch (const std::exception& error) {
      std::printf("[  FAILED  ] %s\n            unexpected exception: %s\n", test.name, error.what());
      std::fflush(stdout);
      ++failed;
    }
  }
  std::printf("\n%zu passed, %zu failed, %zu filtered out, %zu total\n", passed, failed, skipped,
              registry().size());
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

ChildProcess spawn_self_child(const std::vector<std::string>& args) {
  std::vector<const char*> argv;
  const std::string program = self_path();
  argv.push_back(program.c_str());
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(nullptr);
  ChildProcess child;
#if defined(_WIN32)
  child.handle = _spawnv(_P_NOWAIT, program.c_str(), argv.data());
#else
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::execv(program.c_str(), const_cast<char* const*>(argv.data()));
    ::_exit(127);
  }
  child.handle = static_cast<std::intptr_t>(pid);
#endif
  return child;
}

int wait_self_child(ChildProcess child) {
  if (!child.valid()) {
    return -1;
  }
#if defined(_WIN32)
  int status = 0;
  const intptr_t result = _cwait(&status, static_cast<intptr_t>(child.handle), 0);
  return result < 0 ? -1 : status;
#else
  int status = 0;
  if (::waitpid(static_cast<pid_t>(child.handle), &status, 0) < 0) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace sncf_test

int main(int argc, char** argv) {
  sncf_test::set_self_path(argv[0]);
  if (argc >= 4 && std::strcmp(argv[1], "--crash-writer") == 0) {
    return sncf_run_crash_writer(argv[2], argv[3]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--fork-child") == 0) {
    extern int sncf_run_fork_child(const std::string& store, const std::string& mode);
    return sncf_run_fork_child(argv[2], argv[3]);
  }
  return sncf_test::run_all(argc, argv);
}
