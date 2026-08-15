cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinycardia_nrf52840)

target_sources(app PRIVATE
  src/main.c
  src/ble_protocol.c
  src/ble_service.c
  src/ecg_processing.c
  src/ecg_processor.c
  src/max30003.c
  src/power_control.c
)

target_include_directories(app PRIVATE include)
