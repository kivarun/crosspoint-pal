add_executable(CompanionGatePowerTest CompanionGatePowerTest.cpp)

target_include_directories(CompanionGatePowerTest PRIVATE
  ../../src
  ../../lib/BleCompanion/include
)

target_link_libraries(CompanionGatePowerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionGatePowerTest)
