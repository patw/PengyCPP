cmake_minimum_required(VERSION 3.16)
project(Pengy VERSION 1.2.13 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# ── CLI ─────────────────────────────────────────────────────────────
add_executable(pengy_cli
    cli/main.cpp
    config.cpp
    chatmanager.cpp
    tools.cpp
    llmclient.cpp
)
target_link_libraries(pengy_cli PRIVATE Qt6::Core Qt6::Network)

# ── Web UI ───────────────────────────────────────────────────────────
add_executable(pengy_web
    web/main.cpp
    web/webserver.cpp
    web/webchatworker.cpp
    web/web_resources.qrc
    config.cpp
    chatmanager.cpp
    tools.cpp
    llmclient.cpp
)
target_link_libraries(pengy_web PRIVATE Qt6::Core Qt6::Network)

# ── Desktop GUI ──────────────────────────────────────────────────────
add_executable(pengy
    main.cpp
    config.cpp
    chatmanager.cpp
    tools.cpp
    llmclient.cpp
    chatworker.cpp
    mainwindow.cpp
    chathistory.cpp
    chatview.cpp
    chatinput.cpp
    settingsdialog.cpp
    resources.qrc
)

target_link_libraries(pengy PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

if(NOT APPLE AND NOT WIN32)
    set_target_properties(pengy PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

# ── Tests ──────────────────────────────────────────────────────────
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
    enable_testing()
    add_executable(pengy_tests
        tests.cpp
        config.cpp
        chatmanager.cpp
        tools.cpp
        llmclient.cpp
        web/webserver.cpp
        web/webchatworker.cpp
        web/web_resources.qrc
    )
    target_link_libraries(pengy_tests PRIVATE
        Qt6::Core Qt6::Widgets Qt6::Network Qt6::Test
    )
    # Ensure CLI and web binaries exist before tests run (subprocess tests)
    add_dependencies(pengy_tests pengy_cli pengy_web)
    add_test(NAME PengyTests COMMAND pengy_tests)
endif()
