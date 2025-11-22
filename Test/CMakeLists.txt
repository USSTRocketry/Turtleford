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
add_executable(${TEST_RUNNER}
TestRunner.cpp
TestPb.cpp
)

# ---------- Compiler-specific Configuration ----------
target_compile_features(${TEST_RUNNER} PRIVATE cxx_std_20)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(${TEST_RUNNER} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(${TEST_RUNNER} PRIVATE -fsanitize=address)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(${TEST_RUNNER} PRIVATE /W4 /Zi /RTC1 /EHsc)
endif()

# Link with GoogleTest libraries
target_link_libraries(${TEST_RUNNER} PRIVATE gtest gtest_main gmock gmock_main ${PROJECT_NAME} LibDependency)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})
