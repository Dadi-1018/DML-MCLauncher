cmake_minimum_required(VERSION 3.16)
project(MCLite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MCLite WIN32 src/main.cpp)

target_compile_definitions(MCLite PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(MCLite PRIVATE gdiplus shell32 ole32 user32 gdi32)

if (MSVC)
    target_compile_options(MCLite PRIVATE /EHsc /MT)
    target_link_options(MCLite PRIVATE /SUBSYSTEM:WINDOWS)
else()
    target_link_options(MCLite PRIVATE -mwindows -static-libgcc -static-libstdc++)
endif()

set_target_properties(MCLite PROPERTIES OUTPUT_NAME "MCLite")
