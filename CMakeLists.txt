cmake_minimum_required(VERSION 3.24)
project(jotmail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
find_package(OpenSSL REQUIRED)
find_package(Iconv REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(WEBP IMPORTED_TARGET libwebp)
endif()

# --- dependencies -------------------------------------------------------------

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v7.0.3
  GIT_SHALLOW TRUE)

FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE)

# stb_image and stb_image_resize2, header-only.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)

# vmime has no recent release; pin a commit.
FetchContent_Declare(vmime
  GIT_REPOSITORY https://github.com/kisli/vmime
  GIT_TAG 63ddb3b378487e7209ec3390cb33c6cde9bdec0a)

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

set(VMIME_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(VMIME_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(VMIME_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(VMIME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VMIME_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(VMIME_HAVE_SASL_SUPPORT OFF CACHE BOOL "" FORCE)
set(VMIME_CHARSETCONV_LIB "iconv" CACHE STRING "" FORCE)
set(VMIME_HAVE_TLS_SUPPORT ON CACHE BOOL "" FORCE)
set(VMIME_TLS_SUPPORT_LIB "openssl" CACHE STRING "" FORCE)
set(VMIME_HAVE_MESSAGING_PROTO_POP3 OFF CACHE BOOL "" FORCE)
set(VMIME_HAVE_MESSAGING_PROTO_SMTP OFF CACHE BOOL "" FORCE)
set(VMIME_HAVE_MESSAGING_PROTO_MAILDIR OFF CACHE BOOL "" FORCE)
set(VMIME_HAVE_MESSAGING_PROTO_SENDMAIL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(ftxui tomlplusplus vmime stb)

# --- jotmail ------------------------------------------------------------------

add_library(jotmail_core STATIC
  src/clipboard.cpp src/config.cpp src/convert.cpp src/editor.cpp src/html.cpp src/kitty.cpp src/mime.cpp
  src/notes.cpp src/store.cpp src/text.cpp src/theme.cpp)
target_include_directories(jotmail_core PUBLIC src)
target_compile_definitions(jotmail_core PUBLIC VMIME_STATIC)  # vmime-static does not export it
target_link_libraries(jotmail_core PUBLIC
  tomlplusplus::tomlplusplus
  vmime-static OpenSSL::SSL OpenSSL::Crypto Iconv::Iconv ZLIB::ZLIB ftxui::screen)
target_include_directories(jotmail_core SYSTEM PRIVATE ${stb_SOURCE_DIR})
if(WEBP_FOUND)
  target_compile_definitions(jotmail_core PRIVATE JOTMAIL_HAVE_WEBP)
  target_link_libraries(jotmail_core PRIVATE PkgConfig::WEBP)
endif()

add_executable(jotmail src/main.cpp src/tui.cpp)
target_link_libraries(jotmail PRIVATE jotmail_core ftxui::component ftxui::dom ftxui::screen)

# --- tests --------------------------------------------------------------------

option(JOTMAIL_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
if(JOTMAIL_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest
      GIT_TAG v1.17.0
      GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(jotmail_tests test/conversion_test.cpp test/notes_test.cpp)
  target_compile_definitions(jotmail_tests PRIVATE JOTMAIL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
  target_link_libraries(jotmail_tests PRIVATE jotmail_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(jotmail_tests)
endif()
