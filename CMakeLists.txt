cmake_minimum_required(VERSION 3.16)
project(DreamMinecraftLauncher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DML WIN32 src/main.cpp)

target_compile_definitions(DML PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(DML PRIVATE gdiplus shell32 ole32 user32 gdi32)

if (MSVC)
    target_compile_options(DML PRIVATE /EHsc /MT)
    target_link_options(DML PRIVATE /SUBSYSTEM:WINDOWS)
else()
    target_link_options(DML PRIVATE -mwindows -static-libgcc -static-libstdc++)
endif()

set_target_properties(DML PROPERTIES OUTPUT_NAME "DML")
