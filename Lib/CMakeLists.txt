# nanopb
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/nanopb/extra")
find_package(Nanopb REQUIRED)

list(APPEND NANOPB_OPTIONS --source-extension=.cpp --cpp-descriptors)
string(REPLACE ";" " " NANOPB_OPTIONS "${NANOPB_OPTIONS}")

nanopb_generate_cpp(TARGET proto RELPATH Tisdale-proto-files
  Tisdale-proto-files/ProtoMain.proto
  Tisdale-proto-files/FlightComputerGroundStationCommunication/RocketGroundCommunication.proto
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
# target_link_libraries(${PROJECT_NAME} PRIVATE HAL)

add_subdirectory(Bricks)
target_link_libraries(${PROJECT_NAME} PRIVATE Bricks)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  # ================== Test dependency =====================
  add_library(LibDependency INTERFACE)

  # Add link dependencies for anything linking LibDependency
  target_link_libraries(LibDependency INTERFACE proto Bricks)
endif()
