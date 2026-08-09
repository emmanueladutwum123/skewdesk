#include "skewdesk/chain.hpp"
#include "skewdesk/parity.hpp"
#include "skewdesk/surface.hpp"
#include "skewdesk/svi.hpp"

#include <benchmark/benchmark.h>

#include <map>
#include <vector>

namespace {

const skewdesk::OptionChain& Chain() {
  static const skewdesk::OptionChain chain = skewdesk::generate_chain(skewdesk::ChainConfig{});
  return chain;
}

std::vector<skewdesk::ParityQuote> PairedSlice() {
  std::map<double, skewdesk::ParityQuote> paired;
  for (const skewdesk::ContractQuote& quote : Chain().expiries[3].quotes) {
    skewdesk::ParityQuote& entry = paired[quote.strike];
    entry.strike = quote.strike;
    if (quote.type == skewdesk::OptionType::Call) {
      entry.call_price = quote.mid;
    } else {
      entry.put_price = quote.mid;
    }
  }
  std::vector<skewdesk::ParityQuote> result;
  for (const auto& entry : paired) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<skewdesk::SviObservation> SliceObservations() {
  std::vector<skewdesk::SviObservation> observations;
  for (int i = 0; i < 21; ++i) {
    const double k = -0.35 + (static_cast<double>(i) / 20.0) * 0.70;
    const double volatility = skewdesk::skew_volatility(skewdesk::SkewParameters{}, k, 0.5);
    observations.push_back(skewdesk::SviObservation{
        .log_moneyness = k, .total_variance = volatility * volatility * 0.5, .weight = 1.0});
  }
  return observations;
}

// One linear regression across a strike ladder. Cheap enough that recovering
// the forward from the market is never the reason to skip doing it.
void BM_ParityFit(benchmark::State& state) {
  const std::vector<skewdesk::ParityQuote> quotes = PairedSlice();
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::fit_forward_and_discount(quotes));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(quotes.size()));
}
BENCHMARK(BM_ParityFit);

// The dominant cost in the pipeline. The outer search is only two-dimensional,
// but it evaluates a full exact least-squares solve at every grid point across
// six refinement passes.
void BM_SviSliceFit(benchmark::State& state) {
  const std::vector<skewdesk::SviObservation> observations = SliceObservations();
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::fit_svi(observations, 0.5));
  }
}
BENCHMARK(BM_SviSliceFit);

// How much of that cost the search grid drives, measured by halving its
// resolution.
void BM_SviSliceFitCoarse(benchmark::State& state) {
  const std::vector<skewdesk::SviObservation> observations = SliceObservations();
  skewdesk::SviFitSettings settings{};
  settings.grid_points = 11;
  settings.refinements = 3;
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::fit_svi(observations, 0.5, settings));
  }
}
BENCHMARK(BM_SviSliceFitCoarse);

void BM_DurrlemanCheck(benchmark::State& state) {
  const skewdesk::SviParameters parameters =
      skewdesk::fit_svi(SliceObservations(), 0.5).parameters;
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::check_butterfly(parameters, -0.6, 0.6));
  }
}
BENCHMARK(BM_DurrlemanCheck);

// End to end: parity recovery, out-of-the-money inversion, and an SVI fit for
// every expiry, plus both arbitrage checks.
void BM_FitFullSurface(benchmark::State& state) {
  const skewdesk::OptionChain& chain = Chain();
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::fit_surface(chain));
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(chain.expiries.size()));
}
BENCHMARK(BM_FitFullSurface);

void BM_GenerateChain(benchmark::State& state) {
  const skewdesk::ChainConfig config{};
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::generate_chain(config));
  }
}
BENCHMARK(BM_GenerateChain);

void BM_SurfaceLookup(benchmark::State& state) {
  const skewdesk::VolSurface surface = skewdesk::fit_surface(Chain());
  double k = -0.2;
  for (auto _ : state) {
    benchmark::DoNotOptimize(surface.volatility_at(0.4, k));
    k += 1e-6;
    if (k > 0.2) {
      k = -0.2;
    }
  }
}
BENCHMARK(BM_SurfaceLookup);

}  // namespace

BENCHMARK_MAIN();
