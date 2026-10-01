add_executable(SlideshowStateTest SlideshowStateTest.cpp)

target_include_directories(SlideshowStateTest PRIVATE ../../src)

target_link_libraries(SlideshowStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SlideshowStateTest)
