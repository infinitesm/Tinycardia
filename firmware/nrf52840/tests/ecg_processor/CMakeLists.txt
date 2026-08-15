# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinycardia_ecg_processor_tests)

target_sources(app PRIVATE
  src/main.c
  ../../src/ecg_processing.c
  ../../src/ecg_processor.c
)

target_include_directories(app PRIVATE ../../include)
