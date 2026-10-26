cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME gsheets)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
include_directories(third_party)
include_directories(duckdb/third_party/httplib)

# oauth_listener.cpp plus the non-DuckDB sources it calls into
# (gsheets_utils.cpp, encoding.cpp). Kept as one variable, rather than
# hand-duplicated file lists, because test/integration/CMakeLists.txt needs
# this exact set to link its standalone oauth_listener_tests binary (see its
# own comment) - a variable can't drift out of sync with itself the way two
# separately maintained lists could.
set(OAUTH_LISTENER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sheets/auth/oauth_listener.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sheets/util/encoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gsheets_utils.cpp)

# Extension sources
set(EXTENSION_SOURCES
    src/gsheets_extension.cpp
    src/gsheets_auth.cpp
    src/gsheets_copy.cpp
    src/gsheets_read.cpp
    src/sheets/auth/bearer_token_auth.cpp
    src/sheets/auth/oauth_auth.cpp
    src/sheets/auth/oauth_token_exchange.cpp
    src/sheets/auth/service_account_auth.cpp
    src/sheets/resources/base.cpp
    src/sheets/resources/values.cpp
    src/sheets/resources/spreadsheet.cpp
    src/sheets/transport/http_client.cpp
    src/sheets/transport/duckdb_http_client.cpp
    src/sheets/transport/mock_http_client.cpp
    src/sheets/transport/client_factory.cpp
    src/sheets/range.cpp
    src/sheets/auth_factory.cpp
    src/utils/secret.cpp
    src/utils/options.cpp
    src/utils/version.cpp
    ${OAUTH_LISTENER_SOURCES})

# Browsers have no sockets, so DuckDB-WASM builds leave out the local OAuth
# redirect listener (browser logins aren't supported there - see
# gsheets_auth.cpp).
if(EMSCRIPTEN)
  list(REMOVE_ITEM EXTENSION_SOURCES
       ${CMAKE_CURRENT_SOURCE_DIR}/src/sheets/auth/oauth_listener.cpp)
endif()

# Warn on unused/dead code (GCC/Clang only; MSVC uses different flag syntax)
add_compile_options($<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wunused-function>)
add_compile_options($<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wunreachable-code>)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Service-account JWT signing and the PKCE challenge use DuckDB's bundled
# mbedtls. DuckDB builds it with hidden visibility, so the extension links its
# own static copy rather than resolving it from the host (as parquet does).
target_link_libraries(${EXTENSION_NAME} duckdb_mbedtls)
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_mbedtls)

# The local OAuth listener (src/sheets/auth/oauth_listener.cpp) uses the
# vendored httplib::Server, which uses Winsock on Windows. httplib.hpp's
# #pragma comment(lib, "ws2_32.lib") only takes effect for MSVC, so link
# ws2_32 explicitly here for other toolchains (e.g. MinGW/Ninja generators),
# matching test/integration/CMakeLists.txt.
if(WIN32)
  target_link_libraries(${EXTENSION_NAME} ws2_32)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ws2_32)
endif()

# oauth_listener.cpp's Catch2 tests (test/unit/sheets/auth/test_oauth_listener.cpp)
# can't link into the standalone test/unit target (see its CMakeLists.txt),
# so they're built here instead, as part of the full extension build - same
# condition DuckDB itself uses to decide whether to build its own unittest
# runner.
if(${BUILD_UNITTESTS})
  add_subdirectory(test/integration)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
