// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test framework.
//
// It has no third-party dependencies, it never terminates the process on a
// failed assertion, and it imposes no timeout of any kind. A test that hangs is
// a defect to be diagnosed, not a test to be killed.

#ifndef SITE_FABRIC_TESTS_TEST_FRAMEWORK_HPP
#define SITE_FABRIC_TESTS_TEST_FRAMEWORK_HPP

#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sftest {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

[[nodiscard]] std::vector<TestCase>& registry();

int register_test(const char* suite, const char* name, void (*function)());

/// Records a failure for the currently running test.
void record_failure(const char* file, int line, const std::string& message);

/// Detects a library to_string overload found by argument-dependent lookup, so
/// that every strongly typed enum in Site Fabric renders by name.
template <class T, class = void>
struct has_to_string : std::false_type {};

template <class T>
struct has_to_string<T, std::void_t<decltype(to_string(std::declval<const T&>()))>>
    : std::true_type {};

template <class T, class = void>
struct has_member_to_string : std::false_type {};

template <class T>
struct has_member_to_string<T, std::void_t<decltype(std::declval<const T&>().to_string())>>
    : std::true_type {};

[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(const char* value);
[[nodiscard]] std::string describe(bool value);

template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (has_to_string<T>::value) {
    return std::string(to_string(value));
  } else if constexpr (has_member_to_string<T>::value) {
    return std::string(value.to_string());
  } else {
    std::ostringstream out;
    out << value;
    return out.str();
  }
}

int run_all(const std::vector<std::string>& filters, std::size_t repeat, bool list_only);

/// The seed the current property test is running under.
void set_current_seed(unsigned long long seed);
[[nodiscard]] unsigned long long current_seed();

}  // namespace sftest

#define SF_TEST(suite, name)                                                                static void suite##_##name##_body();                                                      static const int suite##_##name##_registration =                                              sftest::register_test(#suite, #name, suite##_##name##_body);                          static void suite##_##name##_body()

#define SF_CHECK(expression)                                                                do {                                                                                        if (!(expression)) {                                                                        sftest::record_failure(__FILE__, __LINE__, "SF_CHECK failed: " #expression);            }                                                                                       } while (false)

#define SF_CHECK_EQ(expected, actual)                                                       do {                                                                                        const auto sf_expected_value = (expected);                                                const auto sf_actual_value = (actual);                                                    if (!(sf_expected_value == sf_actual_value)) {                                              sftest::record_failure(__FILE__, __LINE__,                                                                       std::string("SF_CHECK_EQ failed: " #expected " == " #actual) + \
                                 " expected=" + sftest::describe(sf_expected_value) +                                      " actual=" + sftest::describe(sf_actual_value));             }                                                                                       } while (false)

#define SF_CHECK_NE(left, right)                                                            do {                                                                                        if ((left) == (right)) {                                                                    sftest::record_failure(__FILE__, __LINE__,                                                                       "SF_CHECK_NE failed: " #left " != " #right);                     }                                                                                       } while (false)

#define SF_REQUIRE(expression)                                                              do {                                                                                        if (!(expression)) {                                                                        sftest::record_failure(__FILE__, __LINE__, "SF_REQUIRE failed: " #expression);            return;                                                                                 }                                                                                       } while (false)

#endif  // SITE_FABRIC_TESTS_TEST_FRAMEWORK_HPP
