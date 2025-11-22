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
