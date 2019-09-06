find_package(Boost REQUIRED)
find_package(Seastar REQUIRED)

rp_test(
  UNIT_TEST
  BINARY_NAME log_segment_appender_test
  SOURCES log_segment_appender_test.cc
  LIBRARIES Seastar::seastar_testing storage
  DEFINITIONS SEASTAR_TESTING_MAIN
  ARGS "-- -c 1"
)

rp_test(
  UNIT_TEST
  BINARY_NAME log_segment_test
  SOURCES log_segment_test.cc
  LIBRARIES Seastar::seastar_testing storage
  DEFINITIONS SEASTAR_TESTING_MAIN
  ARGS "-- -c 1"
)

rp_test(
  UNIT_TEST
  BINARY_NAME log_writer_test
  SOURCES log_writer_test.cc
  LIBRARIES Seastar::seastar_testing storage
  DEFINITIONS SEASTAR_TESTING_MAIN
  ARGS "-- -c 1"
)

rp_test(
  UNIT_TEST
  BINARY_NAME parser_test
  SOURCES parser_test.cc
  LIBRARIES Seastar::seastar_testing storage
  DEFINITIONS SEASTAR_TESTING_MAIN
  ARGS "-- -c 1"
)