add_executable(CompanionGateStateTest CompanionGateStateTest.cpp)

target_include_directories(CompanionGateStateTest PRIVATE
  ../../src
)

target_link_libraries(CompanionGateStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionGateStateTest)
