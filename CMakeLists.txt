cmake_minimum_required(VERSION 3.16)
project(WindowHost VERSION 1.0.0 LANGUAGES CXX RC)

add_executable(WindowHost WIN32
    src/main.cpp
    src/pane.cpp
    src/appearance.cpp
    src/settings.cpp
    src/target.cpp
    src/WindowHost.rc
)
target_include_directories(WindowHost PRIVATE src)
set_target_properties(WindowHost PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF)
target_compile_definitions(WindowHost PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000005
)
if(MSVC)
    set_property(TARGET WindowHost PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(WindowHost PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/W4>)
elseif(MINGW)
    target_link_options(WindowHost PRIVATE -municode)
endif()
target_link_libraries(WindowHost PRIVATE user32 gdi32 shell32 ole32 uuid dwmapi advapi32)
