cmake_minimum_required(VERSION 3.20)

project(TaskbarIconOnly VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(TaskbarIconOnly WIN32
    src/main.cpp
)

add_executable(TaskbarIconOnlyInspect
    tests/inspect.cpp
)

target_compile_definitions(TaskbarIconOnly PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(TaskbarIconOnly PRIVATE
    advapi32
    comctl32
    dwmapi
    ole32
    propsys
    shell32
    shlwapi
    user32
)

target_link_libraries(TaskbarIconOnlyInspect PRIVATE
    dwmapi
    ole32
    propsys
    shell32
    shlwapi
    user32
)

if (MINGW)
    target_link_libraries(TaskbarIconOnly PRIVATE winpthread)
    target_link_options(TaskbarIconOnlyInspect PRIVATE -municode)
    target_link_options(TaskbarIconOnly PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -municode
    )
endif()
