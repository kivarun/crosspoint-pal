add_executable(CompanionGatePoll150Test CompanionGatePoll150Test.cpp)

target_include_directories(CompanionGatePoll150Test PRIVATE
  ../../src
  ../../lib/BleCompanion/include
)

target_compile_definitions(CompanionGatePoll150Test PRIVATE
  CROSSPOINT_COMPANION_GATE_POLL_MS=150
)

target_link_libraries(CompanionGatePoll150Test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionGatePoll150Test)
