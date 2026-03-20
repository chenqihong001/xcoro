cmake_minimum_required(VERSION 3.16)
project(xcoro VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)

add_library(xcoro INTERFACE)
add_library(xcoro::xcoro ALIAS xcoro)

target_compile_features(xcoro INTERFACE cxx_std_20)

target_include_directories(xcoro
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(xcoro INTERFACE Threads::Threads)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  option(XCORO_BUILD_TESTS "Build xcoro tests" ON)
else()
  option(XCORO_BUILD_TESTS "Build xcoro tests" OFF)
endif()

if(XCORO_BUILD_TESTS)
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
  target_link_libraries(xcoro_test PRIVATE xcoro::xcoro GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(xcoro_test)
endif()

install(TARGETS xcoro EXPORT xcoroTargets)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/xcoroConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/xcoroConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/xcoroConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xcoro
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/xcoroConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/xcoroConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xcoro
)

install(
  EXPORT xcoroTargets
  FILE xcoroTargets.cmake
  NAMESPACE xcoro::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xcoro
)
