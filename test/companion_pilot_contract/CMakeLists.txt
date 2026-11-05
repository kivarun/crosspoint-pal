add_executable(CompanionPilotContractTest CompanionPilotContractTest.cpp)

target_include_directories(CompanionPilotContractTest PRIVATE
  ../../src
)

target_compile_definitions(CompanionPilotContractTest PRIVATE
  REPO_ROOT_PATH="${REPO_ROOT}"
)

target_link_libraries(CompanionPilotContractTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionPilotContractTest)
