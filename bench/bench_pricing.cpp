#include "skewdesk/black_scholes.hpp"

#include <benchmark/benchmark.h>

namespace {

skewdesk::ForwardInputs AtTheMoney() {
  return skewdesk::ForwardInputs{.forward = 4500.0,
                                 .strike = 4500.0,
                                 .discount_factor = 0.989,
                                 .volatility = 0.18,
                                 .time = 0.25};
}

void BM_NormalCdf(benchmark::State& state) {
  double x = -3.0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::norm_cdf(x));
    x += 1e-6;
    if (x > 3.0) {
      x = -3.0;
    }
  }
}
BENCHMARK(BM_NormalCdf);

void BM_Price(benchmark::State& state) {
  skewdesk::ForwardInputs in = AtTheMoney();
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::price(in, skewdesk::OptionType::Call));
    in.strike += 1e-6;
  }
}
BENCHMARK(BM_Price);

// The deterministic branch: no logarithm, no error function, just a discounted
// max. Included to show what the transcendentals actually cost.
void BM_PriceDegenerate(benchmark::State& state) {
  skewdesk::ForwardInputs in = AtTheMoney();
  in.time = 0.0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::price(in, skewdesk::OptionType::Call));
    in.strike += 1e-6;
  }
}
BENCHMARK(BM_PriceDegenerate);

void BM_ForwardVega(benchmark::State& state) {
  skewdesk::ForwardInputs in = AtTheMoney();
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::forward_vega(in));
    in.strike += 1e-6;
  }
}
BENCHMARK(BM_ForwardVega);

// All five sensitivities from one set of moments. Compare against BM_Price to
// see how much the full greek set costs over a bare valuation.
void BM_Greeks(benchmark::State& state) {
  skewdesk::BlackScholesInputs in{.spot = 4500.0,
                                  .strike = 4500.0,
                                  .rate = 0.042,
                                  .dividend = 0.013,
                                  .volatility = 0.18,
                                  .time = 0.25};
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::greeks(in, skewdesk::OptionType::Call));
    in.strike += 1e-6;
  }
}
BENCHMARK(BM_Greeks);

}  // namespace

BENCHMARK_MAIN();
