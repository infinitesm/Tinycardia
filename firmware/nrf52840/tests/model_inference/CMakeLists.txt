cmake_minimum_required(VERSION 3.20.0)

set(TFLITE_MICRO_MODULE_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/tflite-micro)
if(NOT EXISTS ${TFLITE_MICRO_MODULE_DIR}/zephyr/module.yml)
  message(FATAL_ERROR
    "TFLite Micro dependency is missing. Run: git submodule update --init --recursive")
endif()
list(APPEND ZEPHYR_EXTRA_MODULES ${TFLITE_MICRO_MODULE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinycardia_model_inference_tests)

set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

set(TINYCARDIA_MODEL_FILE
    ${CMAKE_CURRENT_LIST_DIR}/../../../../model/afib_detector_int8.tflite)
set(TINYCARDIA_MODEL_INC ${PROJECT_BINARY_DIR}/afib_detector_int8.inc)
set(TINYCARDIA_MODEL_SHA256
    85a9a57433d27c7a059edb1f5a4752c965d2f0d745e23dddd530fddf320c50b6)
if(NOT EXISTS ${TINYCARDIA_MODEL_FILE})
  message(FATAL_ERROR "Canonical model artifact is missing: ${TINYCARDIA_MODEL_FILE}")
endif()
file(SHA256 ${TINYCARDIA_MODEL_FILE} TINYCARDIA_MODEL_ACTUAL_SHA256)
if(NOT TINYCARDIA_MODEL_ACTUAL_SHA256 STREQUAL TINYCARDIA_MODEL_SHA256)
  message(FATAL_ERROR
    "Canonical model SHA-256 mismatch: expected ${TINYCARDIA_MODEL_SHA256}, got ${TINYCARDIA_MODEL_ACTUAL_SHA256}")
endif()
generate_inc_file_for_target(app ${TINYCARDIA_MODEL_FILE} ${TINYCARDIA_MODEL_INC})

target_sources(app PRIVATE
  src/main.c
  ../../src/model_data.cc
  ../../src/model_inference.cc
)

target_include_directories(app PRIVATE ../../include ${PROJECT_BINARY_DIR})
