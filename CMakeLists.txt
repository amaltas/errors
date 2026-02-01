cmake_minimum_required(VERSION 3.14)
project(errors LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(errors error.cc)
target_include_directories(errors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(error_test error_test.cc)
target_link_libraries(error_test errors GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(error_test)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(error_benchmark error_benchmark.cc)
target_link_libraries(error_benchmark errors benchmark::benchmark_main)
