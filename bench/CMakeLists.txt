# Faza 17.3: benchmark executables (see docs/design.md §23). Not
# GoogleTest-based — see IMPLEMENTATION_PLAN.md's Faza 17 section: these
# are measurement tools, validated by physical plausibility of their
# output, not pass/fail assertions. Links `client` directly (in-process
# Client, so ClientCache state survives across the many reads/writes each
# experiment issues — a fresh dfs_client subprocess per operation would
# lose the cache between every call, see docs/design.md §22) and reuses
# tests/failure/'s real-process harness (`failure_test_support`, Faza 16,
# extended rather than relocated in Faza 17.1 — see docs/design.md §21).

add_library(bench_common bench_common.cpp)
target_link_libraries(bench_common PUBLIC client failure_test_support)

add_executable(bench_sequential_read bench_sequential_read.cpp)
target_link_libraries(bench_sequential_read PRIVATE bench_common)

add_executable(bench_random_read bench_random_read.cpp)
target_link_libraries(bench_random_read PRIVATE bench_common)
