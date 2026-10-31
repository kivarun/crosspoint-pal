add_executable(CompanionGatePolicyTest CompanionGatePolicyTest.cpp)

target_include_directories(CompanionGatePolicyTest PRIVATE
  ../../src
)

target_link_libraries(CompanionGatePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionGatePolicyTest)
