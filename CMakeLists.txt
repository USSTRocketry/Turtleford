cmake_minimum_required(VERSION 3.28)

project(Turtleford VERSION 0.1 LANGUAGES C CXX)

add_library(${PROJECT_NAME} STATIC)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}
    PROPERTIES
    SOVERSION 0
    VERSION 0.1
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        Include
)

add_subdirectory(Src)
add_subdirectory(Lib)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    add_subdirectory(Test)
endif()

# configure strict compiler warnings across the project
if(NOT TARGET global_warning_options)
    add_library(global_warning_options INTERFACE)
    if(MSVC)
        target_compile_options(global_warning_options INTERFACE /W4 /wd4100 /wd4514)
    else()
        # GNU/Clang/AppleClang
        target_compile_options(global_warning_options INTERFACE
            -Wall -Wextra -Wpedantic -Wshadow -Wdouble-promotion -Wundef
        )
    endif()
endif()

# propagate warnings to core target
target_link_libraries(${PROJECT_NAME} PUBLIC global_warning_options)