cmake_minimum_required(VERSION 3.22)

project(tbot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TBOT_BUILD_OPENWRT_ADMIN "Build the OpenWrt administration backend" OFF)

option(TBOT_USE_SYSTEM_JSON "Use system nlohmann/json headers instead of fetching them" OFF)

if(TBOT_USE_SYSTEM_JSON)
    find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    add_library(nlohmann_json_headers INTERFACE)
    target_include_directories(nlohmann_json_headers INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_headers)
else()
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED IMPORTED_TARGET libmicrohttpd>=0.9.71)

add_library(tiny2fa STATIC
    third_party/tiny2fa/tiny2fa.c
    third_party/tiny2fa/base32.c
    third_party/tiny2fa/hmac.c
    third_party/tiny2fa/sha1.c
)
target_include_directories(tiny2fa PUBLIC third_party/tiny2fa/include PRIVATE third_party/tiny2fa/include/google_pam)
set_target_properties(tiny2fa PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.c)
target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)
set_target_properties(qrcodegen PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_library(tbot_core
    src/qr_code.cpp
    src/two_factor.cpp
    src/application.cpp
    src/commands.cpp
    src/config.cpp
    src/notification_service.cpp
    src/notification_http.cpp
    src/script_runner.cpp
    src/telegram_client.cpp
)

target_include_directories(tbot_core PUBLIC include)
target_link_libraries(
    tbot_core
    PUBLIC tiny2fa qrcodegen CURL::libcurl nlohmann_json::nlohmann_json Threads::Threads PkgConfig::MICROHTTPD
)
target_compile_options(tbot_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(tbot src/main.cpp)
target_link_libraries(tbot PRIVATE tbot_core)

add_executable(tbot-send src/tbot_send.cpp)
target_include_directories(tbot-send PRIVATE include)
target_compile_options(tbot-send PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS tbot tbot-send RUNTIME DESTINATION bin)
install(FILES third_party/tiny2fa/LICENSE
    DESTINATION share/licenses/tbot RENAME Tiny2FA-MIT.txt)
install(FILES third_party/tiny2fa/include/google_pam/LICENSE
    DESTINATION share/licenses/tbot RENAME Google-PAM-Apache-2.0.txt)
install(FILES third_party/qrcodegen/LICENSE.txt
    DESTINATION share/licenses/tbot RENAME QRCodeGenerator-MIT.txt)

if(TBOT_BUILD_OPENWRT_ADMIN)
    add_library(tbot_wrt_admin src/openwrt/admin.cpp)
    target_link_libraries(tbot_wrt_admin PUBLIC tbot_core)
    target_compile_options(tbot_wrt_admin PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(tbot-wrt-admin src/openwrt/admin_main.cpp)
    target_link_libraries(tbot-wrt-admin PRIVATE tbot_wrt_admin)
    target_compile_options(tbot-wrt-admin PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS tbot-wrt-admin RUNTIME DESTINATION libexec)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(notification-http-tests tests/notification_http_tests.cpp)
    target_link_libraries(notification-http-tests PRIVATE tbot_core)
    target_compile_options(notification-http-tests PRIVATE -UNDEBUG)
    add_test(NAME notification-http COMMAND notification-http-tests)
    if(TBOT_BUILD_OPENWRT_ADMIN)
        add_executable(admin-tests tests/openwrt/admin_tests.cpp)
        target_link_libraries(admin-tests PRIVATE tbot_wrt_admin)
        target_compile_options(admin-tests PRIVATE -UNDEBUG)
        add_test(NAME admin-configuration COMMAND admin-tests)
    endif()
    add_executable(disabled-callback-tests tests/disabled_callback_tests.cpp)
    target_link_libraries(disabled-callback-tests PRIVATE tbot_core)
    target_compile_options(disabled-callback-tests PRIVATE -UNDEBUG)
    add_test(NAME disabled-command-callbacks COMMAND disabled-callback-tests)
    add_executable(qr-code-tests tests/qr_code_tests.cpp)
    target_link_libraries(qr-code-tests PRIVATE tbot_core)
    target_compile_options(qr-code-tests PRIVATE -UNDEBUG)
    add_test(NAME qr-code-tests COMMAND qr-code-tests)
    add_executable(two-factor-tests tests/two_factor_tests.cpp)
    target_link_libraries(two-factor-tests PRIVATE tbot_core)
    target_compile_options(two-factor-tests PRIVATE -UNDEBUG)
    add_test(NAME two-factor-tests COMMAND two-factor-tests)
    add_executable(tbot-tests tests/core_tests.cpp)
    target_link_libraries(tbot-tests PRIVATE tbot_core)
    add_test(NAME tbot-tests COMMAND tbot-tests)
    add_executable(tbot-send-tests tests/tbot_send_tests.cpp)
    target_compile_options(tbot-send-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME tbot-send-socket COMMAND tbot-send-tests $<TARGET_FILE:tbot-send>)
    add_test(NAME tbot-send-help COMMAND tbot-send --help)
    add_test(NAME tbot-generate-2fa-short
        COMMAND tbot -g 123456789 -a)
    set_tests_properties(tbot-generate-2fa-short PROPERTIES
        PASS_REGULAR_EXPRESSION "\"user_id\": 123456789")
    add_test(NAME tbot-generate-2fa-long
        COMMAND tbot --generate-2fa-secret 987654321 --qr-ascii)
    set_tests_properties(tbot-generate-2fa-long PROPERTIES
        PASS_REGULAR_EXPRESSION "tbot-wrt:987654321")
    add_test(NAME tbot-generate-2fa-invalid-user
        COMMAND tbot -g invalid)
    set_tests_properties(tbot-generate-2fa-invalid-user PROPERTIES WILL_FAIL TRUE)
    add_test(NAME tbot-qr-ascii-without-generate
        COMMAND tbot -a)
    set_tests_properties(tbot-qr-ascii-without-generate PROPERTIES WILL_FAIL TRUE)
endif()
