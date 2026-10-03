add_executable(Rtc32kDiagTest
  Rtc32kDiagTest.cpp
  ${REPO_ROOT}/src/util/Rtc32kDiagnosticsPolicy.h
)

target_include_directories(Rtc32kDiagTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(Rtc32kDiagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Rtc32kDiagTest)
