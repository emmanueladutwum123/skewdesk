if(NOT SD_BUILD_BENCHMARKS)
  return()
endif()

# Benchmarks are compile-checked in CI on the Release legs with -Werror, but
# never run there: a shared runner produces numbers that are noisy and not
# representative, so every figure in BENCHMARKS.md comes from running these
# directly on a quiet machine before pushing.
function(sd_add_benchmark name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE skewdesk benchmark::benchmark)
  target_compile_options(${name} PRIVATE ${SD_WARNING_FLAGS})
endfunction()

sd_add_benchmark(bench_pricing)
sd_add_benchmark(bench_implied_vol)
sd_add_benchmark(bench_surface)
sd_add_benchmark(bench_risk)
sd_add_benchmark(bench_quoting)
sd_add_benchmark(bench_simulation)
