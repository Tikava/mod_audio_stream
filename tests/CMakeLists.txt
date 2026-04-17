cmake_minimum_required(VERSION 3.18)
project(mod_audio_stream_tests CXX)
set(CMAKE_CXX_STANDARD 11)

enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
# Keep gtest/gmock static; don't install alongside the module
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ── test_base64 ─────────────────────────────────────────────────────────────
add_executable(test_base64
    test_base64.cpp
    ../base64.cpp
)
target_include_directories(test_base64 PRIVATE ..)
target_link_libraries(test_base64 PRIVATE GTest::gtest_main)
gtest_discover_tests(test_base64)

# ── test_audio_processing ────────────────────────────────────────────────────
add_executable(test_audio_processing
    test_audio_processing.cpp
)
target_include_directories(test_audio_processing PRIVATE ..)
target_link_libraries(test_audio_processing PRIVATE GTest::gtest_main)
gtest_discover_tests(test_audio_processing)

# ── test_ws_uri ───────────────────────────────────────────────────────────────
add_executable(test_ws_uri
    test_ws_uri.cpp
)
target_include_directories(test_ws_uri PRIVATE ..)
target_link_libraries(test_ws_uri PRIVATE GTest::gtest_main)
gtest_discover_tests(test_ws_uri)
