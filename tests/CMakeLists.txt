include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571 # v1.14.0 — the family pin
)
# MSVC: match the runtime library of the code under test.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(test_soundfile test_soundfile.cpp)
set_target_properties(test_soundfile PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(test_soundfile PRIVATE tap::soundfile GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test_soundfile)
