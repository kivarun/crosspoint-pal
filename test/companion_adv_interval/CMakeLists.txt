add_executable(CompanionAdvIntervalTest
  CompanionAdvIntervalTest.cpp
)

target_include_directories(CompanionAdvIntervalTest PRIVATE
  ../../lib/BleCompanion/src
)

target_compile_definitions(CompanionAdvIntervalTest PRIVATE
  REPO_ROOT_PATH="${REPO_ROOT}"
)

target_link_libraries(CompanionAdvIntervalTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionAdvIntervalTest)
