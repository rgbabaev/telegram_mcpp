include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(TELEGRAM_MCPP_TDLIB_REVISION "23383aef218a3123f02b744bfa91bde21056a814")
FetchContent_Declare(tdlib
    URL "https://codeload.github.com/tdlib/td/tar.gz/${TELEGRAM_MCPP_TDLIB_REVISION}"
    URL_HASH SHA256=322517e8b7ab6b7035a998f52890d6f13c7a06909b879e30484cf806140929cc
)
function(telegram_mcpp_add_tdlib)
    set(BUILD_TESTING OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    FetchContent_MakeAvailable(tdlib)
endfunction()

telegram_mcpp_add_tdlib()
