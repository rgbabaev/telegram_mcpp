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

add_library(telegram_mcpp_core
    src/config.cpp
    src/private_storage.cpp
    src/logger.cpp
)
target_include_directories(telegram_mcpp_core PUBLIC include)
target_link_libraries(telegram_mcpp_core PRIVATE Threads::Threads)
target_compile_options(telegram_mcpp_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(telegram-mcpp src/main.cpp)
target_link_libraries(telegram-mcpp PRIVATE telegram_mcpp_core)
target_compile_options(telegram-mcpp PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(TELEGRAM_MCPP_WITH_TDLIB)
    include(cmake/TDLib.cmake)
    add_library(telegram_mcpp_tdlib src/tdlib.cpp)
    target_include_directories(telegram_mcpp_tdlib PUBLIC include)
    target_link_libraries(telegram_mcpp_tdlib PRIVATE Td::TdStatic)
    target_link_libraries(telegram-mcpp PRIVATE telegram_mcpp_tdlib)
    target_compile_definitions(telegram-mcpp PRIVATE TELEGRAM_MCPP_WITH_TDLIB)
endif()

if(BUILD_TESTING)
    add_executable(bootstrap_tests tests/bootstrap_tests.cpp)
    target_link_libraries(bootstrap_tests PRIVATE telegram_mcpp_core)
    target_compile_options(bootstrap_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME bootstrap COMMAND bootstrap_tests)
    add_test(NAME cli COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:telegram-mcpp> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
    if(TELEGRAM_MCPP_WITH_TDLIB)
        add_test(NAME tdlib_typed_api COMMAND telegram-mcpp --version)
        set_tests_properties(tdlib_typed_api PROPERTIES PASS_REGULAR_EXPRESSION "TDLib [0-9]+\\.[0-9]+")
    endif()
endif()
