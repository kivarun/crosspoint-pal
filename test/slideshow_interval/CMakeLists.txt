add_executable(SlideshowIntervalTest SlideshowIntervalTest.cpp)

target_include_directories(SlideshowIntervalTest PRIVATE
  ../../src
  ../../lib/GfxRenderer  # ToneLut.h via SlideshowPolicy.h
)

target_link_libraries(SlideshowIntervalTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SlideshowIntervalTest)
