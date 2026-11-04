add_executable(MscDiagCoreTest MscDiagCoreTest.cpp)

target_include_directories(MscDiagCoreTest PRIVATE
  ../../src
)

target_link_libraries(MscDiagCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MscDiagCoreTest)
