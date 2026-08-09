#include "skewdesk/chain.hpp"
#include "skewdesk/quoting.hpp"
#include "skewdesk/surface.hpp"

#include <benchmark/benchmark.h>

#include <vector>

namespace {

const skewdesk::VolSurface& Surface() {
  static const skewdesk::VolSurface surface =
      skewdesk::fit_surface(skewdesk::generate_chain(skewdesk::ChainConfig{}));
  return surface;
}

const skewdesk::PortfolioRisk& Risk() {
  static const skewdesk::PortfolioRisk risk = [] {
    skewdesk::PositionBook book;
    for (int i = 0; i < 200; ++i) {
      const skewdesk::SurfaceSlice& slice =
          Surface().slices[static_cast<std::size_t>(i) % Surface().slices.size()];
      book.add(skewdesk::ContractId{.time = slice.time,
                                    .strike = 4000.0 + 25.0 * static_cast<double>(i % 40),
                                    .type = (i % 2 == 0) ? skewdesk::OptionType::Call
                                                         : skewdesk::OptionType::Put},
               (i % 3 == 0) ? -4.0 : 6.0);
    }
    return skewdesk::compute_risk(book, Surface());
  }();
  return risk;
}

std::vector<skewdesk::ContractId> Ladder(int size) {
  std::vector<skewdesk::ContractId> ladder;
  ladder.reserve(static_cast<std::size_t>(size));
  for (int i = 0; i < size; ++i) {
    const skewdesk::SurfaceSlice& slice =
        Surface().slices[static_cast<std::size_t>(i) % Surface().slices.size()];
    ladder.push_back(skewdesk::ContractId{
        .time = slice.time,
        .strike = 3800.0 + 25.0 * static_cast<double>(i % 50),
        .type = (i % 2 == 0) ? skewdesk::OptionType::Call : skewdesk::OptionType::Put});
  }
  return ladder;
}

// The hot path. Everything a market maker does between market updates comes
// down to repeating this: read the surface, look up the inventory bucket,
// widen, skew, convert to price.
void BM_MakeQuote(benchmark::State& state) {
  const skewdesk::ContractId contract{
      .time = 0.25, .strike = 4600.0, .type = skewdesk::OptionType::Call};
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::make_quote(contract, Surface(), Risk()));
  }
}
BENCHMARK(BM_MakeQuote);

void BM_QuoteLadder(benchmark::State& state) {
  const std::vector<skewdesk::ContractId> ladder =
      Ladder(static_cast<int>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::quote_ladder(ladder, Surface(), Risk()));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_QuoteLadder)->Arg(20)->Arg(100)->Arg(500);

}  // namespace

BENCHMARK_MAIN();
