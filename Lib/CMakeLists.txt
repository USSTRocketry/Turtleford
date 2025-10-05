# nanopb
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/nanopb/extra")
find_package(Nanopb REQUIRED)

nanopb_generate_cpp(TARGET proto RELPATH Tisdale-proto-files
  Tisdale-proto-files/Entry.proto
  Tisdale-proto-files/FlightComputerGroundStationCommunication/RocketGroundCommunication.proto
)

target_link_libraries(${PROJECT_NAME} proto)
