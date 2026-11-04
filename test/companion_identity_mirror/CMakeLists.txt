add_executable(CompanionIdentityMirrorTest CompanionIdentityMirrorTest.cpp)

target_include_directories(CompanionIdentityMirrorTest PRIVATE
  ../../src
  ../../lib/BleCompanion/src
)

target_compile_definitions(CompanionIdentityMirrorTest PRIVATE
  REPO_ROOT_PATH="${REPO_ROOT}"
)

target_link_libraries(CompanionIdentityMirrorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionIdentityMirrorTest)
