# Real-VM variants of bench/'s 5 experiments — see remote_common.h and
# docs/benchmark_results_vm.md's methodology header. Not GoogleTest-based,
# same rationale as ../bench_common.h (measurement tools, not pass/fail
# assertions).

add_library(bench_remote_common remote_common.cpp)
target_link_libraries(bench_remote_common PUBLIC bench_common)

add_executable(bench_remote_sequential_read bench_remote_sequential_read.cpp)
target_link_libraries(bench_remote_sequential_read PRIVATE bench_remote_common)

add_executable(bench_remote_random_read bench_remote_random_read.cpp)
target_link_libraries(bench_remote_random_read PRIVATE bench_remote_common)

add_executable(bench_remote_cache_on_off bench_remote_cache_on_off.cpp)
target_link_libraries(bench_remote_cache_on_off PRIVATE bench_remote_common)

add_executable(bench_remote_replication_factor bench_remote_replication_factor.cpp)
target_link_libraries(bench_remote_replication_factor PRIVATE bench_remote_common)

add_executable(bench_remote_node_failure bench_remote_node_failure.cpp)
target_link_libraries(bench_remote_node_failure PRIVATE bench_remote_common)
