add_executable(BleCompanionTest
  BleCompanionTest.cpp
  ${REPO_ROOT}/lib/BleCompanion/src/BleCompanionJson.cpp
)

target_include_directories(BleCompanionTest PRIVATE
  ${REPO_ROOT}/lib/BleCompanion/src
  ${REPO_ROOT}/lib/BleCompanion/include
)

target_link_libraries(BleCompanionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleCompanionTest)
