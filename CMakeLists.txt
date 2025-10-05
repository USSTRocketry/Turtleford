cmake_minimum_required(VERSION 3.28)

project(Turtleford VERSION 0.1 LANGUAGES C CXX)
add_executable(${PROJECT_NAME})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME}
    PROPERTIES
    SOVERSION 0
    VERSION 0.1
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_subdirectory(Src)
add_subdirectory(Lib)
