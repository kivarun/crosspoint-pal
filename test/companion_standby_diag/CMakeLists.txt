add_executable(CompanionStandbyDiagTest CompanionStandbyDiagTest.cpp)

target_include_directories(CompanionStandbyDiagTest PRIVATE
  ../../src
)

target_link_libraries(CompanionStandbyDiagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionStandbyDiagTest)
