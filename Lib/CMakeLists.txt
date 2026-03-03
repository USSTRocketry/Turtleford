# nanopb
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/nanopb/extra")
find_package(Nanopb REQUIRED)

list(APPEND NANOPB_OPTIONS --source-extension=.cpp --cpp-descriptors)
string(REPLACE ";" " " NANOPB_OPTIONS "${NANOPB_OPTIONS}")

nanopb_generate_cpp(TARGET proto RELPATH Tisdale-proto-files
  Tisdale-proto-files/ProtoMain.proto
  Tisdale-proto-files/RocketGroundCommunication.proto
)
target_compile_definitions(proto
 PRIVATE
   PB_BUFFER_ONLY
)

target_include_directories(proto
    PUBLIC ${NANOPB_INCLUDE_DIRS} ${CMAKE_CURRENT_BINARY_DIR} # <- important for generated files
)

# =================== LIB dependencies ======================
target_link_libraries(${PROJECT_NAME} PRIVATE proto)

add_subdirectory(Bricks)
target_link_libraries(${PROJECT_NAME} PRIVATE Bricks)

# Hardcode HAL platform for this repo
set(USST_PLATFORM "MOCK" CACHE STRING "Hard coding HAL")
add_subdirectory(HAL)
target_link_libraries(${PROJECT_NAME} PRIVATE HAL)

# ================== Test dependency =====================
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  add_library(LibDependency INTERFACE)

  # Add link dependencies for anything linking LibDependency
  target_link_libraries(LibDependency INTERFACE proto Bricks HAL)
endif()
