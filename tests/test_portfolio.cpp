#include "skewdesk/portfolio.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

constexpr double kSpot = 4500.0;

// A slice with b = 0 is flat in strike: total variance is exactly `a`, so
// implied volatility is sqrt(a / T) everywhere. That makes the surface's
// volatility directly controllable, which is what lets vega be checked against
// a finite difference below.
SurfaceSlice FlatSlice(double time, double volatility, double rate = 0.042) {
  SurfaceSlice slice{};
  slice.time = time;
  slice.forward = kSpot * std::exp(rate * time);
  slice.discount_factor = std::exp(-rate * time);
  slice.k_low = -0.5;
  slice.k_high = 0.5;
  slice.fit.time = time;
  slice.fit.status = SviFitStatus::Success;
  slice.fit.parameters =
      SviParameters{.a = volatility * volatility * time, .b = 0.0, .rho = 0.0, .m = 0.0,
                    .sigma = 0.2};
  return slice;
}

VolSurface FlatSurface(double volatility = 0.20) {
  VolSurface surface{};
  surface.spot = kSpot;
  surface.slices = {FlatSlice(0.08, volatility), FlatSlice(0.25, volatility),
                    FlatSlice(1.0, volatility), FlatSlice(2.0, volatility)};
  surface.status = SurfaceStatus::Success;
  return surface;
}

TEST(PositionBook, NetsRepeatedAddsToTheSameContract) {
  PositionBook book;
  const ContractId call{.time = 0.25, .strike = 4500.0, .type = OptionType::Call};

  book.add(call, 10.0);
  book.add(call, -4.0);

  EXPECT_EQ(book.size(), 1u);
  EXPECT_DOUBLE_EQ(book.quantity_of(call), 6.0);
}

TEST(PositionBook, DropsContractsNettedFlat) {
  PositionBook book;
  const ContractId call{.time = 0.25, .strike = 4500.0, .type = OptionType::Call};

  book.add(call, 7.0);
  book.add(call, -7.0);

  EXPECT_TRUE(book.empty());
  EXPECT_DOUBLE_EQ(book.quantity_of(call), 0.0);
}

TEST(PositionBook, TreatsCallsAndPutsAtOneStrikeAsDifferentContracts) {
  PositionBook book;
  const ContractId call{.time = 0.25, .strike = 4500.0, .type = OptionType::Call};
  const ContractId put{.time = 0.25, .strike = 4500.0, .type = OptionType::Put};

  book.add(call, 5.0);
  book.add(put, -3.0);

  EXPECT_EQ(book.size(), 2u);
  EXPECT_DOUBLE_EQ(book.quantity_of(call), 5.0);
  EXPECT_DOUBLE_EQ(book.quantity_of(put), -3.0);
}

TEST(PositionBook, BucketEdgesAreHalfOpenFromBelow) {
  const std::vector<double> edges = {-0.10, -0.03, 0.03, 0.10};

  EXPECT_EQ(bucket_index(-0.50, edges), 0u);
  EXPECT_EQ(bucket_index(-0.10, edges), 1u) << "an edge belongs to the bucket above it";
  EXPECT_EQ(bucket_index(0.00, edges), 2u);
  EXPECT_EQ(bucket_index(0.03, edges), 3u);
  EXPECT_EQ(bucket_index(0.50, edges), 4u);
}

TEST(Portfolio, SinglePositionMatchesTheScaledContractGreeks) {
  const VolSurface surface = FlatSurface();
  const ContractId contract{.time = 1.0, .strike = 4700.0, .type = OptionType::Call};

  PositionBook book;
  book.add(contract, 12.0);

  RiskSettings settings{};
  settings.multiplier = 100.0;
  const PortfolioRisk risk = compute_risk(book, surface, settings);
  ASSERT_EQ(risk.positions_priced, 1);
  ASSERT_EQ(risk.positions_skipped, 0);

  const double forward = surface.forward_at(contract.time);
  const ForwardInputs in{
      .forward = forward,
      .strike = contract.strike,
      .discount_factor = surface.discount_factor_at(contract.time),
      .volatility = surface.volatility_at(contract.time, std::log(contract.strike / forward)),
      .time = contract.time};
  const Greeks expected = greeks(from_forward(surface.spot, in), contract.type);
  const double scale = 12.0 * 100.0;

  EXPECT_NEAR(risk.value, scale * price(in, contract.type), 1e-9);
  EXPECT_NEAR(risk.delta, scale * expected.delta, 1e-9);
  EXPECT_NEAR(risk.gamma, scale * expected.gamma, 1e-12);
  EXPECT_NEAR(risk.vega, scale * expected.vega, 1e-9);
  EXPECT_NEAR(risk.theta, scale * expected.theta, 1e-9);
}

TEST(Portfolio, RiskIsAdditiveAndLinearInQuantity) {
  const VolSurface surface = FlatSurface();
  const ContractId call{.time = 0.25, .strike = 4600.0, .type = OptionType::Call};
  const ContractId put{.time = 1.0, .strike = 4300.0, .type = OptionType::Put};

  PositionBook only_call;
  only_call.add(call, 5.0);
  PositionBook only_put;
  only_put.add(put, -8.0);
  PositionBook both;
  both.add(call, 5.0);
  both.add(put, -8.0);

  const PortfolioRisk a = compute_risk(only_call, surface);
  const PortfolioRisk b = compute_risk(only_put, surface);
  const PortfolioRisk combined = compute_risk(both, surface);

  EXPECT_NEAR(combined.value, a.value + b.value, 1e-9);
  EXPECT_NEAR(combined.delta, a.delta + b.delta, 1e-9);
  EXPECT_NEAR(combined.vega, a.vega + b.vega, 1e-9);
  EXPECT_NEAR(combined.theta, a.theta + b.theta, 1e-9);

  PositionBook doubled;
  doubled.add(call, 10.0);
  const PortfolioRisk scaled = compute_risk(doubled, surface);
  EXPECT_NEAR(scaled.vega, 2.0 * a.vega, 1e-9);
  EXPECT_NEAR(scaled.delta, 2.0 * a.delta, 1e-9);
}

// Vega checked against a numerical derivative of the book's own mark, by
// shifting a flat surface's volatility. Validates the aggregation and the
// scaling together, not just the per-contract formula.
TEST(Portfolio, VegaMatchesAFiniteDifferenceOfTheBookMark) {
  PositionBook book;
  book.add(ContractId{.time = 0.25, .strike = 4400.0, .type = OptionType::Put}, -20.0);
  book.add(ContractId{.time = 1.0, .strike = 4700.0, .type = OptionType::Call}, 35.0);

  const double base_vol = 0.20;
  const double h = 1e-5;

  const PortfolioRisk risk = compute_risk(book, FlatSurface(base_vol));
  const double up = compute_risk(book, FlatSurface(base_vol + h)).value;
  const double down = compute_risk(book, FlatSurface(base_vol - h)).value;

  EXPECT_NEAR(risk.vega, (up - down) / (2.0 * h), 1e-3);
}

// The reason bucketed vega exists. This book is long front-month vega and
// short two-year vega in almost exactly offsetting amounts: net vega is
// negligible, and the position is anything but flat.
TEST(Portfolio, NetVegaHidesAnOffsettingTermStructurePosition) {
  const VolSurface surface = FlatSurface();

  const ContractId front{.time = 0.08, .strike = 4500.0, .type = OptionType::Call};
  const ContractId back{.time = 2.0, .strike = 4500.0, .type = OptionType::Call};

  PositionBook probe;
  probe.add(front, 1.0);
  const double front_vega = compute_risk(probe, surface).vega;
  probe.clear();
  probe.add(back, 1.0);
  const double back_vega = compute_risk(probe, surface).vega;

  ASSERT_GT(front_vega, 0.0);
  ASSERT_GT(back_vega, front_vega) << "longer-dated options carry more vega";

  PositionBook book;
  book.add(front, 1000.0);
  book.add(back, -1000.0 * front_vega / back_vega);

  const PortfolioRisk risk = compute_risk(book, surface);
  // front_vega already carries the contract multiplier, since it came back
  // from compute_risk rather than from a bare per-contract greek.
  const double scale = std::fabs(1000.0 * front_vega);

  EXPECT_NEAR(risk.vega, 0.0, 1e-6 * scale) << "net vega should be ~flat";
  EXPECT_GT(risk.gross_vega, 1.9 * scale)
      << "gross vega should reveal the risk the net figure hides";

  // And the two exposures land in different tenor buckets, with opposite signs.
  const std::vector<double>& by_tenor = risk.vega_buckets.by_tenor;
  const std::size_t front_bucket = bucket_index(front.time, RiskBucketConfig{}.tenor_edges);
  const std::size_t back_bucket = bucket_index(back.time, RiskBucketConfig{}.tenor_edges);
  ASSERT_NE(front_bucket, back_bucket);
  EXPECT_GT(by_tenor[front_bucket], 0.0);
  EXPECT_LT(by_tenor[back_bucket], 0.0);
}

TEST(Portfolio, VegaBucketsSumToNetVega) {
  const VolSurface surface = FlatSurface();

  PositionBook book;
  book.add(ContractId{.time = 0.08, .strike = 4200.0, .type = OptionType::Put}, 15.0);
  book.add(ContractId{.time = 0.25, .strike = 4500.0, .type = OptionType::Call}, -22.0);
  book.add(ContractId{.time = 1.0, .strike = 5000.0, .type = OptionType::Call}, 9.0);
  book.add(ContractId{.time = 2.0, .strike = 3900.0, .type = OptionType::Put}, -6.0);

  const PortfolioRisk risk = compute_risk(book, surface);
  ASSERT_EQ(risk.positions_priced, 4);

  double tenor_total = 0.0;
  for (const double cell : risk.vega_buckets.by_tenor) {
    tenor_total += cell;
  }
  double moneyness_total = 0.0;
  for (const double cell : risk.vega_buckets.by_log_moneyness) {
    moneyness_total += cell;
  }
  double grid_total = 0.0;
  for (const std::vector<double>& row : risk.vega_buckets.grid) {
    for (const double cell : row) {
      grid_total += cell;
    }
  }

  EXPECT_NEAR(tenor_total, risk.vega, 1e-9);
  EXPECT_NEAR(moneyness_total, risk.vega, 1e-9);
  EXPECT_NEAR(grid_total, risk.vega, 1e-9);
  EXPECT_GE(risk.gross_vega, std::fabs(risk.vega) - 1e-9);
}

// A vega point at three weeks is not the same risk as one at two years,
// because short-dated implied volatility moves far more.
TEST(Portfolio, TermWeightingUpweightsShortDatedVega) {
  const VolSurface surface = FlatSurface();
  RiskSettings settings{};
  settings.weighted_vega_reference_time = 0.25;

  PositionBook front;
  front.add(ContractId{.time = 0.08, .strike = 4500.0, .type = OptionType::Call}, 10.0);
  const PortfolioRisk short_dated = compute_risk(front, surface, settings);

  PositionBook back;
  back.add(ContractId{.time = 2.0, .strike = 4500.0, .type = OptionType::Call}, 10.0);
  const PortfolioRisk long_dated = compute_risk(back, surface, settings);

  EXPECT_GT(short_dated.weighted_vega, short_dated.vega);
  EXPECT_LT(long_dated.weighted_vega, long_dated.vega);
  EXPECT_NEAR(short_dated.weighted_vega, short_dated.vega * std::sqrt(0.25 / 0.08), 1e-9);
  EXPECT_NEAR(long_dated.weighted_vega, long_dated.vega * std::sqrt(0.25 / 2.0), 1e-9);
}

TEST(Portfolio, StraddleIsDeltaLightAndGammaHeavy) {
  const VolSurface surface = FlatSurface();
  const double atm_strike = surface.forward_at(0.25);

  PositionBook book;
  book.add(ContractId{.time = 0.25, .strike = atm_strike, .type = OptionType::Call}, 10.0);
  book.add(ContractId{.time = 0.25, .strike = atm_strike, .type = OptionType::Put}, 10.0);

  const PortfolioRisk risk = compute_risk(book, surface);

  // Struck at the forward, the two deltas nearly cancel; they do not cancel
  // exactly because spot delta differs from forward delta by the carry.
  EXPECT_LT(std::fabs(risk.delta), 0.10 * std::fabs(risk.vega));
  EXPECT_GT(risk.gamma, 0.0);
  EXPECT_GT(risk.vega, 0.0);
  EXPECT_LT(risk.theta, 0.0) << "a long straddle decays";
}

TEST(Portfolio, ForwardAndDiscountInterpolateAtConstantRates) {
  VolSurface surface{};
  surface.spot = kSpot;
  surface.slices = {FlatSlice(1.0, 0.2, 0.05), FlatSlice(2.0, 0.2, 0.05)};
  surface.status = SurfaceStatus::Success;

  // A single continuously-compounded rate across both nodes means the log of
  // each quantity is exactly linear in maturity, including back to zero.
  EXPECT_NEAR(surface.forward_at(0.0), kSpot, 1e-9);
  EXPECT_NEAR(surface.forward_at(0.5), kSpot * std::exp(0.05 * 0.5), 1e-9);
  EXPECT_NEAR(surface.forward_at(1.5), kSpot * std::exp(0.05 * 1.5), 1e-9);
  EXPECT_NEAR(surface.forward_at(3.0), kSpot * std::exp(0.05 * 3.0), 1e-9)
      << "beyond the last node the final segment's rate continues";

  EXPECT_NEAR(surface.discount_factor_at(0.0), 1.0, 1e-12);
  EXPECT_NEAR(surface.discount_factor_at(0.5), std::exp(-0.05 * 0.5), 1e-9);
  EXPECT_NEAR(surface.discount_factor_at(3.0), std::exp(-0.05 * 3.0), 1e-9);
}

TEST(Portfolio, MalformedPositionsAreSkippedAndCounted) {
  const VolSurface surface = FlatSurface();

  PositionBook book;
  book.add(ContractId{.time = 0.0, .strike = 4500.0, .type = OptionType::Call}, 5.0);
  book.add(ContractId{.time = -1.0, .strike = 4500.0, .type = OptionType::Call}, 5.0);
  book.add(ContractId{.time = 1.0, .strike = 4500.0, .type = OptionType::Call}, 5.0);

  const PortfolioRisk risk = compute_risk(book, surface);
  EXPECT_EQ(risk.positions_priced, 1);
  EXPECT_EQ(risk.positions_skipped, 2);
}

TEST(Portfolio, RiskAgainstAnUnusableSurfaceSkipsEverything) {
  VolSurface empty{};

  PositionBook book;
  book.add(ContractId{.time = 1.0, .strike = 4500.0, .type = OptionType::Call}, 5.0);

  const PortfolioRisk risk = compute_risk(book, empty);
  EXPECT_EQ(risk.positions_priced, 0);
  EXPECT_EQ(risk.positions_skipped, 1);
  EXPECT_DOUBLE_EQ(risk.vega, 0.0);
  EXPECT_EQ(risk.vega_buckets.by_tenor.size(), RiskBucketConfig{}.tenor_bucket_count());
}

TEST(Portfolio, PricesABookAgainstASurfaceFittedFromAChain) {
  const VolSurface surface = fit_surface(generate_chain(ChainConfig{}));
  ASSERT_TRUE(surface.ok());

  PositionBook book;
  book.add(ContractId{.time = 0.25, .strike = 4300.0, .type = OptionType::Put}, -50.0);
  book.add(ContractId{.time = 0.25, .strike = 4700.0, .type = OptionType::Call}, 50.0);
  book.add(ContractId{.time = 1.0, .strike = 4500.0, .type = OptionType::Call}, 25.0);

  const PortfolioRisk risk = compute_risk(book, surface);
  EXPECT_EQ(risk.positions_priced, 3);
  EXPECT_EQ(risk.positions_skipped, 0);
  EXPECT_GT(risk.gross_vega, 0.0);
  EXPECT_TRUE(std::isfinite(risk.value));
  EXPECT_TRUE(std::isfinite(risk.delta));
}

}  // namespace
}  // namespace skewdesk
