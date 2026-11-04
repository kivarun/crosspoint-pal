add_executable(RtcClockDiagCoreTest RtcClockDiagCoreTest.cpp)

target_include_directories(RtcClockDiagCoreTest PRIVATE
  ../../src
)

target_link_libraries(RtcClockDiagCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RtcClockDiagCoreTest)
