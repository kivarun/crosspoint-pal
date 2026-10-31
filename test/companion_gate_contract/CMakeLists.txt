add_executable(CompanionGateContractTest CompanionGateContractTest.cpp)

target_include_directories(CompanionGateContractTest PRIVATE
  ../../src
)

target_compile_definitions(CompanionGateContractTest PRIVATE
  REPO_ROOT_PATH="${REPO_ROOT}"
)

target_link_libraries(CompanionGateContractTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionGateContractTest)
