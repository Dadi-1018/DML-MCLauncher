cmake_minimum_required(VERSION 3.16)
project(DreamMinecraftLauncher VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 资源脚本（图标 + 页眉标志）：MSVC 用 rc.exe，MinGW 用 windres，均由 CMake 自动选取
if (WIN32)
    enable_language(RC)
endif()

add_executable(DML WIN32 src/main.cpp)

if (WIN32)
    target_sources(DML PRIVATE res/app.rc)
endif()

target_compile_definitions(DML PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(DML PRIVATE gdiplus shell32 ole32 user32 gdi32 winhttp crypt32 comdlg32 winmm)

if (MSVC)
    target_compile_options(DML PRIVATE /EHsc /MT)
    target_link_options(DML PRIVATE /SUBSYSTEM:WINDOWS)
else()
    target_link_options(DML PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(DML PROPERTIES OUTPUT_NAME "DML")
