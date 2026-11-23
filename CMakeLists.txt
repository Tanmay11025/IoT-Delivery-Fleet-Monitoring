cmake_minimum_required(VERSION 3.16)

project(message_gateway LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(message_gateway
	src/main.cpp
	src/core/config.cpp
	src/http/http_parser.cpp
	src/http/http_response.cpp
	src/http/router.cpp
	src/http/static_files.cpp
	src/protocol/frame_codec.cpp
	src/telemetry/telemetry_event.cpp
	src/telemetry/fleet_state.cpp
	src/security/rate_limiter.cpp
	src/storage/mmap_log.cpp
	src/monitoring/metrics.cpp
	src/network/connection_manager.cpp
	src/network/epoll_loop.cpp
	src/network/tcp_server.cpp
)

target_include_directories(message_gateway PRIVATE src)
target_compile_options(message_gateway PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(message_gateway PRIVATE Threads::Threads)

enable_testing()

function(add_case_tests target)
	foreach(case_name IN LISTS ARGN)
		add_test(NAME ${case_name} COMMAND ${target} ${case_name})
	endforeach()
endfunction()

add_executable(http_parser_tests
	tests/http_parser_test.cpp
	src/http/http_parser.cpp
)

target_include_directories(http_parser_tests PRIVATE src)
target_compile_options(http_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
add_case_tests(http_parser_tests
	parses_fragmented_headers_and_body
	parses_request_without_body
	separates_query_from_request_path
	rejects_malformed_and_unsupported_requests
	enforces_limits
)

add_executable(rate_limiter_tests
	tests/rate_limiter_test.cpp
	src/http/http_parser.cpp
	src/http/http_response.cpp
	src/http/router.cpp
	src/security/rate_limiter.cpp
)

target_include_directories(rate_limiter_tests PRIVATE src)
target_compile_options(rate_limiter_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rate_limiter_tests PRIVATE Threads::Threads)
add_case_tests(rate_limiter_tests
	token_bucket_allows_burst_and_refills
	token_bucket_caps_refill_at_capacity
	token_bucket_does_not_refill_without_elapsed_time
	rate_limiter_tracks_ips_independently
	router_rejects_before_handler_dispatch
	rate_limiter_serializes_concurrent_consumption_per_ip
)

add_executable(metrics_tests
	tests/metrics_test.cpp
	src/monitoring/metrics.cpp
)

target_include_directories(metrics_tests PRIVATE src)
target_compile_options(metrics_tests PRIVATE -Wall -Wextra -Wpedantic)
add_case_tests(metrics_tests
	metrics_track_telemetry_vehicles_and_consumers
	metrics_track_connection_lifecycle
	metrics_track_requests_responses_and_duration
	metrics_track_parse_errors_and_uptime
)

add_executable(config_tests
	tests/config_test.cpp
	src/core/config.cpp
)
target_include_directories(config_tests PRIVATE src)
target_compile_options(config_tests PRIVATE -Wall -Wextra -Wpedantic)
add_case_tests(config_tests
	config_reads_fleet_and_consumer_settings
	config_reads_file_and_cli_overrides
	config_rejects_invalid_values_and_unknown_keys
)

add_executable(thread_pool_tests tests/thread_pool_test.cpp)
target_include_directories(thread_pool_tests PRIVATE src)
target_compile_options(thread_pool_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(thread_pool_tests PRIVATE Threads::Threads)
add_case_tests(thread_pool_tests
	pool_bounds_queue_and_drains_accepted_tasks
	pool_survives_task_exceptions
)

add_executable(tcp_server_drain_tests
	tests/tcp_server_drain_test.cpp
	src/core/config.cpp
	src/protocol/frame_codec.cpp
	src/telemetry/telemetry_event.cpp
	src/telemetry/fleet_state.cpp
	src/storage/mmap_log.cpp
	src/http/http_parser.cpp
	src/http/http_response.cpp
	src/http/router.cpp
	src/security/rate_limiter.cpp
	src/monitoring/metrics.cpp
	src/network/connection_manager.cpp
	src/network/epoll_loop.cpp
	src/network/tcp_server.cpp
)
target_include_directories(tcp_server_drain_tests PRIVATE src)
target_compile_options(tcp_server_drain_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tcp_server_drain_tests PRIVATE Threads::Threads)
add_case_tests(tcp_server_drain_tests
	shutdown_drains_an_in_flight_handler
)

add_executable(mmap_log_tests
	tests/mmap_log_test.cpp
	src/storage/mmap_log.cpp
)
target_include_directories(mmap_log_tests PRIVATE src)
target_compile_options(mmap_log_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(mmap_log_tests PRIVATE Threads::Threads)
add_case_tests(mmap_log_tests
	mmap_log_recovers_records_after_reopen
	mmap_log_rolls_segments_and_tracks_global_offsets
	mmap_log_truncates_a_torn_active_tail_and_resumes
	mmap_log_discards_latest_record_with_bad_checksum
	mmap_log_rejects_records_that_cannot_fit
	mmap_log_truncates_payload_left_before_record_header
	mmap_log_truncates_a_record_with_a_partial_payload
	mmap_log_fails_on_corruption_in_a_sealed_segment
	mmap_log_reads_from_an_offset_across_segments
)

add_executable(storage_recovery_verify
	tests/storage_recovery_verify.cpp
	src/storage/mmap_log.cpp
)
target_include_directories(storage_recovery_verify PRIVATE src)
target_compile_options(storage_recovery_verify PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(storage_recovery_verify PRIVATE Threads::Threads)

add_executable(circular_buffer_tests tests/circular_buffer_test.cpp)
target_include_directories(circular_buffer_tests PRIVATE src)
target_compile_options(circular_buffer_tests PRIVATE -Wall -Wextra -Wpedantic)
add_case_tests(circular_buffer_tests
	circular_buffer_preserves_order_before_wraparound
	circular_buffer_returns_last_values_after_wraparound
	circular_buffer_handles_exact_capacity_and_zero_count
	circular_buffer_supports_capacity_one
)

add_executable(telemetry_event_tests
	tests/telemetry_event_test.cpp
	src/telemetry/telemetry_event.cpp
)
target_include_directories(telemetry_event_tests PRIVATE src)
target_compile_options(telemetry_event_tests PRIVATE -Wall -Wextra -Wpedantic)
add_case_tests(telemetry_event_tests
	telemetry_accepts_a_valid_event
	telemetry_accepts_whitespace_order_and_unknown_scalar_fields
	telemetry_decodes_string_escapes
	telemetry_rejects_missing_and_mistyped_fields
	telemetry_rejects_out_of_range_values
	telemetry_rejects_malformed_json
)

add_executable(fleet_state_tests
	tests/fleet_state_test.cpp
	src/telemetry/telemetry_event.cpp
	src/telemetry/fleet_state.cpp
)
target_include_directories(fleet_state_tests PRIVATE src)
target_compile_options(fleet_state_tests PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(fleet_state_tests PRIVATE Threads::Threads)
add_case_tests(fleet_state_tests
	fleet_state_keeps_the_latest_report_per_vehicle
	fleet_state_ignores_reports_that_arrive_out_of_order
	fleet_state_marks_silent_vehicles_offline
	fleet_state_restores_age_when_rebuilt_from_the_log
	fleet_state_json_lists_vehicles_and_counts
	fleet_state_handles_concurrent_updates
)

add_executable(frame_codec_tests
	tests/frame_codec_test.cpp
	src/protocol/frame_codec.cpp
)
target_include_directories(frame_codec_tests PRIVATE src)
target_compile_options(frame_codec_tests PRIVATE -Wall -Wextra -Wpedantic)
add_case_tests(frame_codec_tests
	frame_codec_encodes_big_endian_frames
	frame_codec_reassembles_fragmented_frames
	frame_codec_splits_several_frames_from_one_read
	frame_codec_rejects_invalid_frames
)
