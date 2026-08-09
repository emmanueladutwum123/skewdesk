#include "skewdesk/simulation.hpp"

#include <benchmark/benchmark.h>

namespace {

skewdesk::SimulationConfig Config(int steps) {
  skewdesk::SimulationConfig config{};
  config.steps = steps;
  config.expiries = {0.15, 0.5, 1.0};
  config.strikes_per_expiry = 7;
  return config;
}

// Per-step cost of the full loop: rebuild the surface, mark the book, quote a
// ladder, match flow, hedge, advance the market, attribute the P&L, and mark
// out matured trades. Reported per step so the number stays comparable across
// run lengths.
void BM_SimulationStep(benchmark::State& state) {
  const skewdesk::SimulationConfig config = Config(static_cast<int>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::run_simulation(config));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SimulationStep)->Arg(50)->Arg(250)->Unit(benchmark::kMillisecond);

// Surface rebuilding dominates the loop, so a coarser fit is the lever that
// matters if a run needs to be longer.
void BM_SimulationStepCoarseFit(benchmark::State& state) {
  skewdesk::SimulationConfig config = Config(static_cast<int>(state.range(0)));
  config.fit.grid_points = 11;
  config.fit.refinements = 3;
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::run_simulation(config));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SimulationStepCoarseFit)->Arg(250)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
