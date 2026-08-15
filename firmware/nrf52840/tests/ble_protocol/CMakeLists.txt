# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinycardia_ble_protocol_tests)

target_sources(app PRIVATE
  src/main.c
  ../../src/ble_protocol.c
)

target_include_directories(app PRIVATE ../../include)
