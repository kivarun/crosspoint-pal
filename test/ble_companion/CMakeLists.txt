add_executable(BleCompanionTest
  BleCompanionTest.cpp
  ${REPO_ROOT}/lib/BleCompanion/src/BleCompanionJson.cpp
  ${REPO_ROOT}/lib/BleCompanion/src/CompanionPeer.cpp
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

add_executable(BleCompanionSourceContractTest
  SourceContractTest.cpp
  ${REPO_ROOT}/lib/BleCompanion/src/BleCompanionJson.cpp
  ${REPO_ROOT}/lib/BleCompanion/src/CompanionPeer.cpp
)

target_include_directories(BleCompanionSourceContractTest PRIVATE
  ${REPO_ROOT}/lib/BleCompanion/src
  ${REPO_ROOT}/lib/BleCompanion/include
)

target_compile_definitions(BleCompanionSourceContractTest PRIVATE
  BLECOMPANION_ROOT_PATH="${REPO_ROOT}/lib/BleCompanion"
)

target_link_libraries(BleCompanionSourceContractTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BleCompanionSourceContractTest)
