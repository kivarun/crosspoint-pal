add_executable(ImageLabToneTest
  ImageLabToneTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/ToneLut.cpp
)

target_link_libraries(ImageLabToneTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImageLabToneTest)
