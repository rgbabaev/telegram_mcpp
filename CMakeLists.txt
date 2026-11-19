cmake_minimum_required(VERSION 3.22)
project(telegram_mcpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "telegram_mcpp requires Linux")
endif()

option(TELEGRAM_MCPP_WITH_TDLIB "Build the pinned TDLib typed API adapter" ON)
include(CTest)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(telegram_mcpp_core
    src/config.cpp
    src/private_storage.cpp
    src/logger.cpp
    src/text.cpp
    src/cursor.cpp
    src/read_json.cpp
    src/mcp_protocol.cpp
    src/mcp_stdio.cpp
    src/telegram_operations.cpp
    src/account.cpp
    src/operator_terminal.cpp
)
target_include_directories(telegram_mcpp_core PUBLIC include)
target_include_directories(telegram_mcpp_core SYSTEM PRIVATE third_party)
target_link_libraries(telegram_mcpp_core PRIVATE Threads::Threads OpenSSL::Crypto)
target_compile_options(telegram_mcpp_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(telegram-mcpp src/main.cpp)
target_link_libraries(telegram-mcpp PRIVATE telegram_mcpp_core)
target_compile_options(telegram-mcpp PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(TELEGRAM_MCPP_WITH_TDLIB)
    include(cmake/TDLib.cmake)
    add_library(telegram_mcpp_tdlib src/tdlib.cpp src/tdlib_read.cpp src/tdlib_draft.cpp)
    target_include_directories(telegram_mcpp_tdlib PUBLIC include)
    target_include_directories(telegram_mcpp_tdlib SYSTEM PRIVATE $<TARGET_PROPERTY:Td::TdStatic,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_options(telegram_mcpp_tdlib PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(telegram_mcpp_tdlib PRIVATE Td::TdStatic telegram_mcpp_core)
    target_link_libraries(telegram-mcpp PRIVATE telegram_mcpp_tdlib)
    target_compile_definitions(telegram-mcpp PRIVATE TELEGRAM_MCPP_WITH_TDLIB)
endif()

if(BUILD_TESTING)
    add_executable(bootstrap_tests tests/bootstrap_tests.cpp)
    target_link_libraries(bootstrap_tests PRIVATE telegram_mcpp_core)
    target_compile_options(bootstrap_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME bootstrap COMMAND bootstrap_tests)
    add_executable(mcp_fixture tests/mcp_fixture.cpp)
    target_link_libraries(mcp_fixture PRIVATE telegram_mcpp_core)
    target_compile_options(mcp_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_executable(read_tests tests/read_tests.cpp)
    target_link_libraries(read_tests PRIVATE telegram_mcpp_core)
    target_compile_options(read_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME read_operations COMMAND read_tests)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME read_json COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/read_json.py $<TARGET_FILE:read_tests>)
    add_test(NAME mcp_stdio COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_stdio.py $<TARGET_FILE:mcp_fixture>)
    set_tests_properties(mcp_stdio PROPERTIES TIMEOUT 45)
    set(TELEGRAM_MCPP_MCP_CLIENT_PYTHON "" CACHE FILEPATH "Python with tools/mcp-client-requirements.txt installed")
    if(TELEGRAM_MCPP_MCP_CLIENT_PYTHON)
        add_test(NAME mcp_reference_client COMMAND ${TELEGRAM_MCPP_MCP_CLIENT_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/mcp_smoke.py --server $<TARGET_FILE:mcp_fixture>
            --fixture normal --chat-id=-9223372036854775808 --message-id=9223372036854775807 --query private-query)
        add_test(NAME mcp_schemas COMMAND ${TELEGRAM_MCPP_MCP_CLIENT_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_schemas.py $<TARGET_FILE:mcp_fixture>)
        add_test(NAME mcp_latest_message COMMAND ${TELEGRAM_MCPP_MCP_CLIENT_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/mcp_smoke.py --server $<TARGET_FILE:mcp_fixture>
            --fixture latest --chat-id=-9223372036854775808 --message-id=2 --query private-query
            --sender-user-id=9223372036854775807)
        set_tests_properties(mcp_reference_client mcp_schemas mcp_latest_message PROPERTIES TIMEOUT 30)
    endif()
    set_tests_properties(read_operations PROPERTIES TIMEOUT 30)
    add_executable(account_tests tests/account_tests.cpp)
    target_link_libraries(account_tests PRIVATE telegram_mcpp_core)
    target_compile_options(account_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_executable(operator_tests tests/operator_tests.cpp)
    target_link_libraries(operator_tests PRIVATE telegram_mcpp_core)
    target_compile_options(operator_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    find_program(TELEGRAM_MCPP_QRENCODE qrencode)
    if(TELEGRAM_MCPP_QRENCODE)
        add_test(NAME operator_qr COMMAND operator_tests --qr)
        set_tests_properties(operator_qr PROPERTIES TIMEOUT 15)
    endif()
    add_test(NAME operator_terminal COMMAND operator_tests)
    set_tests_properties(operator_terminal PROPERTIES TIMEOUT 25)
    add_test(NAME account COMMAND account_tests)
    set_tests_properties(account PROPERTIES TIMEOUT 20)
    add_test(NAME cli COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:telegram-mcpp> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS telegram-mcpp bootstrap_tests account_tests operator_tests read_tests mcp_fixture
        USES_TERMINAL
    )
    if(TELEGRAM_MCPP_WITH_TDLIB)
        add_executable(tdlib_read_tests tests/tdlib_read_tests.cpp)
        target_link_libraries(tdlib_read_tests PRIVATE telegram_mcpp_tdlib telegram_mcpp_core Td::TdStatic)
        target_include_directories(tdlib_read_tests SYSTEM PRIVATE $<TARGET_PROPERTY:Td::TdStatic,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_options(tdlib_read_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME tdlib_read_mapping COMMAND tdlib_read_tests)
        add_dependencies(check tdlib_read_tests)
        add_executable(tdlib_account_tests tests/tdlib_account_tests.cpp)
        target_link_libraries(tdlib_account_tests PRIVATE telegram_mcpp_tdlib telegram_mcpp_core)
        add_dependencies(check tdlib_account_tests)
        add_test(NAME tdlib_account COMMAND tdlib_account_tests)
        add_test(NAME operator_lifecycle COMMAND operator_tests $<TARGET_FILE:telegram-mcpp>)
        set_tests_properties(operator_lifecycle PROPERTIES TIMEOUT 35)
        set_tests_properties(tdlib_account PROPERTIES TIMEOUT 30)
        add_test(NAME tdlib_typed_api COMMAND telegram-mcpp --version)
        set_tests_properties(tdlib_typed_api PROPERTIES PASS_REGULAR_EXPRESSION "TDLib [0-9]+\\.[0-9]+")
    endif()
endif()
