add_executable(CompanionIdentityMirrorTest CompanionIdentityMirrorTest.cpp)

target_include_directories(CompanionIdentityMirrorTest PRIVATE
  ../../src
  ../../lib/BleCompanion/src
)

target_link_libraries(CompanionIdentityMirrorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionIdentityMirrorTest)
