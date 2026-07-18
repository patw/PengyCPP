cmake_minimum_required(VERSION 3.16)
project(Pengy VERSION 1.3.10 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Generate version.h from template
configure_file(
    ${CMAKE_SOURCE_DIR}/version.h.in
    ${CMAKE_BINARY_DIR}/version.h
    @ONLY
)

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
if(UNIX AND NOT WIN32)
    target_link_libraries(pengy_cli PRIVATE readline)
endif()
target_include_directories(pengy_cli PRIVATE ${CMAKE_BINARY_DIR})

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
target_include_directories(pengy_web PRIVATE ${CMAKE_BINARY_DIR})

# ── Desktop GUI ──────────────────────────────────────────────────────
add_executable(pengy
    main.cpp
    config.cpp
    chatmanager.cpp
    taskmanager.cpp
    tools.cpp
    llmclient.cpp
    chatworker.cpp
    mainwindow.cpp
    chathistory.cpp
    tasksdialog.cpp
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

target_include_directories(pengy PRIVATE ${CMAKE_BINARY_DIR})

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
        taskmanager.cpp
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
