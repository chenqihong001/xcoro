cmake_minimum_required(VERSION 3.16)
project(xcoro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  option(XCORO_BUILD_TESTS "Build xcoro tests" ON)
else()
  option(XCORO_BUILD_TESTS "Build xcoro tests" OFF)
endif()

if(XCORO_BUILD_TESTS)
  find_package(Threads REQUIRED)
  include(FetchContent)

  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
  )

  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  set(TEST_FILES
    tests/generator_test.cpp
    tests/cancellation_test.cpp
    tests/net_test.cpp
    tests/semaphore_test.cpp
    tests/mutex_test.cpp
    tests/condition_variable_test.cpp
    tests/event_test.cpp
    tests/thread_pool_test.cpp
    tests/when_any_test.cpp
    tests/when_all_test.cpp
  )

  add_executable(xcoro_test ${TEST_FILES})
  target_compile_features(xcoro_test PRIVATE cxx_std_20)
  target_include_directories(xcoro_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(xcoro_test PRIVATE Threads::Threads GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(xcoro_test)
endif()
