cmake_minimum_required(VERSION 3.16)
project(Pengy VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

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
