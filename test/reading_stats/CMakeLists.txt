add_executable(ReadingStatsSerializationTest
  ReadingStatsSerializationTest.cpp
  stubs/HalClockStub.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsSerialization.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)

target_include_directories(ReadingStatsSerializationTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # HalClock.h / CrossPointSettings.h need Arduino
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReadingStatsSerializationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsSerializationTest)
