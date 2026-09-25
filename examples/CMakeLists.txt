# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(site_fabric_quick_start quick_start.cpp)
target_link_libraries(site_fabric_quick_start PRIVATE site_fabric site_fabric_warnings)

add_executable(site_fabric_loopback_session loopback_session.cpp)
target_link_libraries(site_fabric_loopback_session PRIVATE site_fabric site_fabric_warnings)
