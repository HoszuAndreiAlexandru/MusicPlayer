cmake_minimum_required(VERSION 3.21)

project(MusicPlayer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Network Widgets)

qt_add_executable(MusicPlayer
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/spotify_connection.cpp
    src/spotify_connection.h
)

target_link_libraries(MusicPlayer PRIVATE Qt6::Network Qt6::Widgets)

if(WIN32)
    target_link_libraries(MusicPlayer PRIVATE Crypt32)
elseif(APPLE)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    target_link_libraries(MusicPlayer PRIVATE "${SECURITY_FRAMEWORK}")
endif()

set_target_properties(MusicPlayer PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt
        HINTS "${Qt6_DIR}/../../../bin"
        REQUIRED
    )

    add_custom_command(TARGET MusicPlayer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:MusicPlayer>"
        COMMENT "Deploying Qt runtime beside MusicPlayer"
        VERBATIM
    )
endif()
