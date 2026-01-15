cmake_minimum_required(VERSION 3.10)
project(Attempt1)

set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
)
# For Windows: Do not override the internal /MT flag with /MD
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  absl
  URL https://github.com/abseil/abseil-cpp/archive/refs/heads/master.zip
)

FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3470000.zip
)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
)

# curl options to keep it light
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  curl
  URL https://github.com/curl/curl/archive/refs/tags/curl-8_5_0.zip
)

FetchContent_MakeAvailable(googletest absl sqlite3 nlohmann_json curl)

# sqlite3 doesn't have a CMakeLists.txt in the amalgamation, so we define it if it's not already defined
if(NOT TARGET sqlite3)
  add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
  # Apply some common SQLite flags
  target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_COLUMN_METADATA SQLITE_ENABLE_FTS5)
endif()

# Disable exceptions
if(MSVC)
    add_compile_options(/EHs-c-)
    add_definitions(-D_HAS_EXCEPTIONS=0)
else()
    add_compile_options(-fno-exceptions)
endif()

option(USE_ASAN "Enable AddressSanitizer" OFF)
if(USE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

find_package(PkgConfig)
pkg_check_modules(READLINE readline)

if(NOT READLINE_FOUND)
    find_library(READLINE_LIBRARY NAMES readline)
    find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h)
    if(READLINE_LIBRARY AND READLINE_INCLUDE_DIR)
        set(READLINE_FOUND TRUE)
        set(READLINE_LIBRARIES ${READLINE_LIBRARY})
        set(READLINE_INCLUDE_DIRS ${READLINE_INCLUDE_DIR})
    endif()
endif()

add_library(sentinel_lib database.cpp http_client.cpp orchestrator.cpp tool_executor.cpp command_handler.cpp ui.cpp completion.cpp)
target_link_libraries(sentinel_lib absl::status absl::statusor sqlite3 libcurl nlohmann_json::nlohmann_json absl::strings absl::time)

if(READLINE_FOUND)
    target_link_libraries(sentinel_lib ${READLINE_LIBRARIES})
    target_include_directories(sentinel_lib PUBLIC ${READLINE_INCLUDE_DIRS})
    add_definitions(-DHAVE_READLINE)
endif()

add_executable(Attempt1 main.cpp)
target_link_libraries(Attempt1 sentinel_lib nlohmann_json::nlohmann_json absl::time)

enable_testing()

add_executable(sqlite_test sqlite_test.cpp)
target_link_libraries(sqlite_test GTest::gtest_main absl::status absl::statusor sqlite3)

add_executable(deps_test deps_test.cpp)
target_link_libraries(deps_test GTest::gtest_main nlohmann_json::nlohmann_json libcurl)

add_executable(database_test database_test.cpp)
target_link_libraries(database_test GTest::gtest_main sentinel_lib nlohmann_json::nlohmann_json)

add_executable(http_client_test http_client_test.cpp)
target_link_libraries(http_client_test GTest::gtest_main sentinel_lib nlohmann_json::nlohmann_json)

add_executable(orchestrator_test orchestrator_test.cpp)
target_link_libraries(orchestrator_test GTest::gtest_main sentinel_lib nlohmann_json::nlohmann_json)

add_executable(tool_executor_test tool_executor_test.cpp)
target_link_libraries(tool_executor_test GTest::gtest_main sentinel_lib nlohmann_json::nlohmann_json)

add_executable(command_handler_test command_handler_test.cpp)
target_link_libraries(command_handler_test GTest::gtest_main sentinel_lib nlohmann_json::nlohmann_json)

add_executable(ui_test ui_test.cpp)
target_link_libraries(ui_test GTest::gtest_main sentinel_lib nlohmann_json::nlohmann_json)

include(GoogleTest)
gtest_discover_tests(sqlite_test)
gtest_discover_tests(deps_test)
gtest_discover_tests(database_test)
gtest_discover_tests(http_client_test)
gtest_discover_tests(orchestrator_test)
gtest_discover_tests(tool_executor_test)
gtest_discover_tests(command_handler_test)
gtest_discover_tests(ui_test)