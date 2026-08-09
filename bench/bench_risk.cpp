#include "skewdesk/chain.hpp"
#include "skewdesk/portfolio.hpp"
#include "skewdesk/surface.hpp"

#include <benchmark/benchmark.h>

namespace {

const skewdesk::VolSurface& Surface() {
  static const skewdesk::VolSurface surface =
      skewdesk::fit_surface(skewdesk::generate_chain(skewdesk::ChainConfig{}));
  return surface;
}

// The strike period, slice index and option type have to advance on
// independent cycles. An earlier version stepped strikes on i % 60 while
// choosing the slice on i % 6; because 60 is a multiple of 6 the pair repeated
// after 60 contracts, the book netted every duplicate away, and compute_risk
// measured flat from 100 positions to 5000 -- a benchmark that was reporting
// its own generator's period rather than the code under test.
constexpr int kStrikePeriod = 400;

skewdesk::PositionBook BookOfSize(int positions) {
  const skewdesk::VolSurface& surface = Surface();
  const auto slice_count = static_cast<int>(surface.slices.size());

  skewdesk::PositionBook book;
  for (int i = 0; i < positions; ++i) {
    const skewdesk::SurfaceSlice& slice = surface.slices[static_cast<std::size_t>(
        (i / kStrikePeriod) % slice_count)];
    const double strike =
        2000.0 + 12.5 * static_cast<double>(i % kStrikePeriod);
    const bool is_call = ((i / (kStrikePeriod * slice_count)) % 2) == 0;
    book.add(skewdesk::ContractId{.time = slice.time,
                                  .strike = strike,
                                  .type = is_call ? skewdesk::OptionType::Call
                                                  : skewdesk::OptionType::Put},
             (i % 3 == 0) ? -5.0 : 7.0);
  }
  return book;
}

// Marking and bucketing a book. Linear in position count, which is what makes
// it cheap enough to recompute before every quote refresh rather than caching
// stale risk.
void BM_ComputeRisk(benchmark::State& state) {
  const skewdesk::PositionBook book = BookOfSize(static_cast<int>(state.range(0)));
  const skewdesk::VolSurface& surface = Surface();

  for (auto _ : state) {
    benchmark::DoNotOptimize(skewdesk::compute_risk(book, surface));
  }
  // Counted from the book's real size rather than the requested one, so a
  // generator that quietly stopped producing distinct contracts would show up
  // in the throughput figure instead of hiding behind it.
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(book.size()));
  state.counters["positions"] = static_cast<double>(book.size());
}
BENCHMARK(BM_ComputeRisk)->Arg(10)->Arg(100)->Arg(1000)->Arg(4000);

// PositionBook::add is a linear scan by contract, so building a book is
// quadratic in its size. Deliberate: books are small, the constant is tiny,
// and a map would cost more per lookup than the scan saves.
void BM_BuildBook(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(BookOfSize(static_cast<int>(state.range(0))));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BuildBook)->Arg(100)->Arg(1000)->Arg(4000);

}  // namespace

BENCHMARK_MAIN();
