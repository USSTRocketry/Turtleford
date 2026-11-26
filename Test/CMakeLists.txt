# Enable testing
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/50b8600c63c5487e901e2845a0f64d384a65f75d.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ------------------- Add your test executable -------------------
set(TEST_RUNNER TestRunner)
add_executable(${TEST_RUNNER})

# ---------- Compiler-specific Configuration ----------
target_compile_features(${TEST_RUNNER} PRIVATE cxx_std_20)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${TEST_RUNNER} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(${TEST_RUNNER} PRIVATE -fsanitize=address)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(${TEST_RUNNER} PRIVATE /W4 /Zi /RTC1 /EHsc)
endif()

# Test helpers are included as "Sim/SimRadio.h", "Mocks/MockTransport.h"
target_include_directories(${TEST_RUNNER} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Link with GoogleTest libraries
target_link_libraries(${TEST_RUNNER} PRIVATE gmock_main ${PROJECT_NAME} LibDependency)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})

# ---------- Test Source files ----------
target_sources(${TEST_RUNNER} PRIVATE
  TestRunner.cpp
  TestPb.cpp
  TestFragment.cpp
  TestSmallRng.cpp
  TestTurnTaking.cpp
  TestLoraTransport.cpp
  TestArq.cpp
  TestLink.cpp
  TestChannelRequest.cpp
  TestRadio.cpp
  TestRadioReliable.cpp
  TestRadioRouting.cpp
  TestRadioChannel.cpp
  TestRadioOverLora.cpp
  TestSimRadio.cpp
  TestRadioSoak.cpp
  TestLoraSoak.cpp
  TestTurtleford.cpp
  TestLog.cpp
)
