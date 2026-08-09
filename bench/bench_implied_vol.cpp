#include "skewdesk/implied_volatility.hpp"

#include <benchmark/benchmark.h>

namespace {

skewdesk::ImpliedVolQuery Query(double strike, double time) {
  return skewdesk::ImpliedVolQuery{
      .forward = 4500.0, .strike = strike, .discount_factor = 0.989, .time = time};
}

// Near the money, vega is large and Newton converges quadratically in a
// handful of steps.
void BM_ImpliedVolAtTheMoney(benchmark::State& state) {
  const skewdesk::ImpliedVolQuery query = Query(4500.0, 0.25);
  const double target =
      skewdesk::price(skewdesk::with_volatility(query, 0.18), skewdesk::OptionType::Call);

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        skewdesk::implied_volatility(target, query, skewdesk::OptionType::Call));
  }
}
BENCHMARK(BM_ImpliedVolAtTheMoney);

// The safeguard's cost, and the reason it exists. At this strike vega is about
// 1.5e-15 while the option is still worth an ordinary 4e-18, so an unguarded
// Newton step divides by approximately zero. The rejection conditions force
// bisection instead, which converges linearly -- roughly forty iterations to
// close the bracket rather than a handful.
void BM_ImpliedVolDeepWing(benchmark::State& state) {
  const skewdesk::ImpliedVolQuery query = Query(7000.0, 0.05);
  const double target =
      skewdesk::price(skewdesk::with_volatility(query, 0.22), skewdesk::OptionType::Call);

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        skewdesk::implied_volatility(target, query, skewdesk::OptionType::Call));
  }
}
BENCHMARK(BM_ImpliedVolDeepWing);

// Rejected before any iteration runs: the quote carries no volatility
// information, so the solver returns immediately rather than burning its
// budget converging on a meaningless answer.
void BM_ImpliedVolIllConditioned(benchmark::State& state) {
  const skewdesk::ImpliedVolQuery query = Query(3000.0, 0.02);
  const double target =
      skewdesk::price(skewdesk::with_volatility(query, 0.30), skewdesk::OptionType::Call);

  for (auto _ : state) {
    benchmark::DoNotOptimize(
        skewdesk::implied_volatility(target, query, skewdesk::OptionType::Call));
  }
}
BENCHMARK(BM_ImpliedVolIllConditioned);

}  // namespace

BENCHMARK_MAIN();
