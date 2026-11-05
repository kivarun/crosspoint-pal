add_executable(CompanionGatePilotTest CompanionGatePilotTest.cpp)

target_include_directories(CompanionGatePilotTest PRIVATE
  ../../src
)

target_link_libraries(CompanionGatePilotTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionGatePilotTest)
