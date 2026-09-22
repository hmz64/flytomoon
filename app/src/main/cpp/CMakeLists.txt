cmake_minimum_required(VERSION 3.22)
project(flytomoon LANGUAGES C CXX)

include(FetchContent)

# Raylib 5.5 for Android (static lib, linked into our NativeActivity .so)
set(PLATFORM Android CACHE STRING "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
)
FetchContent_MakeAvailable(raylib)

add_library(flytomoon SHARED main.cpp)

target_link_libraries(flytomoon
    raylib
    log
    android
    EGL
    GLESv2
    OpenSLES
    m
    c
)
