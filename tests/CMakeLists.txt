# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# The test executables.
#
# No test carries a timeout, and no test is wrapped in one. A hang is a defect
# to diagnose, never something to terminate.

set(SITE_FABRIC_TEST_SOURCES
  test_main.cpp
  test_core.cpp
  test_composition.cpp
  test_capacity.cpp
  test_failure_domains.cpp
  test_maintenance.cpp
  test_authority.cpp
  test_invalidation.cpp
  test_snapshots.cpp
  test_persistence.cpp
  test_protocol.cpp
  test_recovery.cpp
  test_concurrency.cpp
  test_property.cpp
  test_adversarial.cpp
  test_controller.cpp)

add_executable(site_fabric_tests ${SITE_FABRIC_TEST_SOURCES})
# The tests reach into src/ on purpose: the canonical codec, the digest
# helpers and the raw transport are part of what is being tested, and testing
# them through a public wrapper would test the wrapper instead.
target_include_directories(site_fabric_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(site_fabric_tests PRIVATE site_fabric site_fabric_warnings)

add_test(NAME site_fabric_unit COMMAND site_fabric_tests)

# A second run of the whole suite with repeats proves that the runtime starts
# and stops deterministically and that no state leaks between instances.
add_test(NAME site_fabric_unit_repeat COMMAND site_fabric_tests --repeat 3)

if(WIN32)
  # The multiprocess suite starts real processes, so it needs a private
  # translation unit and its own main.
  add_executable(site_fabric_multiprocess_tests test_multiprocess.cpp test_main.cpp)
  target_include_directories(site_fabric_multiprocess_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(site_fabric_multiprocess_tests PRIVATE site_fabric site_fabric_warnings)
  add_test(NAME site_fabric_multiprocess COMMAND site_fabric_multiprocess_tests)
  set_tests_properties(site_fabric_multiprocess PROPERTIES
    ENVIRONMENT "SITE_FABRIC_CONTROLLER=$<TARGET_FILE:site_fabric_controller>;SITE_FABRIC_MEMBER=$<TARGET_FILE:site_fabric_member>")
endif()
