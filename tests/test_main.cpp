// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Test runner.
//
// Usage: site_fabric_tests [--list] [--repeat N] [--suite NAME] [--seed N]
//
// There is no timeout anywhere in this program. If a test hangs, the run hangs,
// and that is the correct behaviour: a hang is a defect to be diagnosed.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "site_fabric/site_fabric.hpp"
#include "test_framework.hpp"

namespace sftest {
namespace {

std::atomic<unsigned long long> g_seed{0};

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void set_current_seed(unsigned long long seed) { g_seed.store(seed); }

unsigned long long current_seed() { return g_seed.load(); }

std::string describe(const std::string& value) { return value; }
std::string describe(const char* value) { return value == nullptr ? "(null)" : value; }
std::string describe(bool value) { return value ? "true" : "false"; }

namespace {

std::string& current_test() {
  static std::string name;
  return name;
}

std::vector<std::string>& failures() {
  static std::vector<std::string> list;
  return list;
}

}  // namespace

void record_failure(const char* file, int line, const std::string& message) {
  std::string entry = file;
  entry += ":";
  entry += std::to_string(line);
  entry += ": ";
  entry += message;
  failures().push_back(std::move(entry));
}

int run_all(const std::vector<std::string>& filters, std::size_t repeat, bool list_only) {
  auto& cases = registry();
  if (list_only) {
    for (const auto& test : cases) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    return 0;
  }

  std::vector<TestCase> selected;
  for (const auto& test : cases) {
    if (filters.empty()) {
      selected.push_back(test);
      continue;
    }
    const std::string full = test.suite + "." + test.name;
    for (const auto& filter : filters) {
      if (full.find(filter) != std::string::npos) {
        selected.push_back(test);
        break;
      }
    }
  }

  if (selected.empty()) {
    std::cerr << "no tests selected\n";
    return 2;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  const auto started = std::chrono::steady_clock::now();

  for (std::size_t round = 0; round < repeat; ++round) {
    for (const auto& test : selected) {
      current_test() = test.suite + "." + test.name;
      const std::size_t before = failures().size();
      // A fresh seed per round and per test keeps every run reproducible from
      // the printed value while still varying the property cases.
      const unsigned long long seed =
          0x9E3779B97F4A7C15ULL * (round + 1) + 0x100000001B3ULL * (passed + failed + 1);
      set_current_seed(seed);
      test.function();
      const std::size_t added = failures().size() - before;
      if (added == 0) {
        ++passed;
        continue;
      }
      ++failed;
      std::cout << "FAIL " << current_test() << " (seed " << seed << ")\n";
      for (std::size_t index = before; index < failures().size(); ++index) {
        std::cout << "  " << failures()[index] << "\n";
      }
      failures().resize(before);
    }
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  std::cout << (failed == 0 ? "PASS" : "FAIL") << " passed=" << passed << " failed=" << failed
            << " selected=" << selected.size() << " repeat=" << repeat << " elapsed_ms=" << elapsed
            << "\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace sftest

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  std::size_t repeat = 1;
  bool list_only = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--repeat" && index + 1 < argc) {
      repeat = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
      if (repeat == 0) {
        repeat = 1;
      }
    } else if (argument.rfind("--suite=", 0) == 0) {
      filters.push_back(argument.substr(8));
    } else if (argument == "--suite" && index + 1 < argc) {
      filters.emplace_back(argv[++index]);
    } else if (argument == "--seed" && index + 1 < argc) {
      sftest::set_current_seed(std::strtoull(argv[++index], nullptr, 10));
    } else if (argument.rfind("--", 0) == 0) {
      std::cerr << "unknown option: " << argument << "\n";
      return 2;
    } else {
      filters.push_back(argument);
    }
  }

  std::cout << site_fabric::build_description() << "\n";
  return sftest::run_all(filters, repeat, list_only);
}
