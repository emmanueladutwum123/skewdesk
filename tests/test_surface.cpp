#include "skewdesk/surface.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

SurfaceSlice MakeSlice(double time, const SviParameters& p, double k_low = -0.4,
                       double k_high = 0.4) {
  SurfaceSlice slice{};
  slice.time = time;
  slice.forward = 4500.0;
  slice.discount_factor = std::exp(-0.042 * time);
  slice.k_low = k_low;
  slice.k_high = k_high;
  slice.fit.parameters = p;
  slice.fit.time = time;
  slice.fit.status = SviFitStatus::Success;
  return slice;
}

// The flagship: a chain generated from a surface the fitter has never seen the
// functional form of, put through parity recovery, inversion, and SVI fitting,
// and required to come out free of both kinds of static arbitrage.
TEST(Surface, FitsAGeneratedChainWithoutArbitrage) {
  const OptionChain chain = generate_chain(ChainConfig{});
  const VolSurface surface = fit_surface(chain);

  ASSERT_EQ(surface.status, SurfaceStatus::Success)
      << "status=" << static_cast<int>(surface.status);
  ASSERT_EQ(surface.slices.size(), chain.expiries.size());

  for (const SurfaceSlice& slice : surface.slices) {
    EXPECT_TRUE(slice.fit.ok()) << "T=" << slice.time;
    EXPECT_TRUE(slice.fit.butterfly.free_of_arbitrage) << "T=" << slice.time;
    EXPECT_GE(slice.observations_used, 5) << "T=" << slice.time;
    EXPECT_NEAR(slice.parity_r_squared, 1.0, 1e-9);
  }

  EXPECT_TRUE(surface.calendar.free_of_arbitrage);
  EXPECT_GE(surface.calendar.worst_gap, 0.0);
}

// The fitted surface has to reproduce the generating surface, not merely be
// self-consistent. Compared in volatility terms because that is the unit the
// error is interpretable in.
TEST(Surface, ReproducesTheGeneratingVolatilitySurface) {
  const ChainConfig config{};
  const VolSurface surface = fit_surface(generate_chain(config));
  ASSERT_TRUE(surface.ok());

  for (const SurfaceSlice& slice : surface.slices) {
    for (const double position : {0.15, 0.35, 0.5, 0.65, 0.85}) {
      const double k = slice.k_low + position * (slice.k_high - slice.k_low);
      const double fitted = svi_volatility(slice.fit.parameters, k, slice.time);
      const double truth = skew_volatility(config.skew, k, slice.time);
      // Measured worst case across the full quoted band is 1.3e-3 absolute
      // volatility, so this is a real bound rather than a generous one.
      EXPECT_NEAR(fitted, truth, 2e-3)
          << "T=" << slice.time << " k=" << k << " fitted=" << fitted
          << " truth=" << truth;
    }
  }
}

TEST(Surface, StaysArbitrageFreeWhenQuotesAreNoisy) {
  ChainConfig config{};
  config.quote_noise_vol_points = 0.004;
  const VolSurface surface = fit_surface(generate_chain(config));

  EXPECT_EQ(surface.status, SurfaceStatus::Success)
      << "status=" << static_cast<int>(surface.status);
  for (const SurfaceSlice& slice : surface.slices) {
    EXPECT_TRUE(slice.fit.butterfly.free_of_arbitrage) << "T=" << slice.time;
  }
}

// Total variance decreasing with maturity at a fixed log-moneyness is a
// calendar spread with negative cost.
TEST(Surface, CalendarCheckDetectsAnInvertedPair) {
  const SviParameters rich{.a = 0.09, .b = 0.10, .rho = -0.3, .m = 0.0, .sigma = 0.2};
  const SviParameters cheap{.a = 0.02, .b = 0.10, .rho = -0.3, .m = 0.0, .sigma = 0.2};

  const std::vector<SurfaceSlice> inverted = {MakeSlice(0.25, rich), MakeSlice(1.0, cheap)};
  const CalendarCheck bad = check_calendar(inverted);
  EXPECT_FALSE(bad.free_of_arbitrage);
  EXPECT_LT(bad.worst_gap, 0.0);
  EXPECT_EQ(bad.earlier_slice, 0u);
  EXPECT_EQ(bad.later_slice, 1u);

  const std::vector<SurfaceSlice> ordered = {MakeSlice(0.25, cheap), MakeSlice(1.0, rich)};
  const CalendarCheck good = check_calendar(ordered);
  EXPECT_TRUE(good.free_of_arbitrage);
  EXPECT_GT(good.worst_gap, 0.0);
}

// A calendar check phrased in implied volatility rather than total variance
// would fire here, even though nothing is wrong: volatility falling with
// maturity is an ordinary term structure, and only total variance has to rise.
TEST(Surface, FallingVolatilityIsNotCalendarArbitrage) {
  const double near_vol = 0.30;
  const double far_vol = 0.20;
  const double near_time = 0.25;
  const double far_time = 1.0;

  const SviParameters near_slice{
      .a = near_vol * near_vol * near_time, .b = 0.0, .rho = 0.0, .m = 0.0, .sigma = 0.2};
  const SviParameters far_slice{
      .a = far_vol * far_vol * far_time, .b = 0.0, .rho = 0.0, .m = 0.0, .sigma = 0.2};

  ASSERT_LT(far_vol, near_vol) << "premise: volatility falls with maturity";

  const std::vector<SurfaceSlice> slices = {MakeSlice(near_time, near_slice),
                                            MakeSlice(far_time, far_slice)};
  const CalendarCheck check = check_calendar(slices);
  EXPECT_TRUE(check.free_of_arbitrage);
}

TEST(Surface, TotalVarianceInterpolatesLinearlyInMaturity) {
  const SviParameters early{.a = 0.02, .b = 0.10, .rho = -0.3, .m = 0.0, .sigma = 0.2};
  const SviParameters late{.a = 0.08, .b = 0.10, .rho = -0.3, .m = 0.0, .sigma = 0.2};

  VolSurface surface{};
  surface.slices = {MakeSlice(0.5, early), MakeSlice(1.5, late)};
  surface.status = SurfaceStatus::Success;

  const double k = 0.1;
  const double w_early = total_variance(early, k);
  const double w_late = total_variance(late, k);

  EXPECT_NEAR(surface.total_variance_at(0.5, k), w_early, 1e-12);
  EXPECT_NEAR(surface.total_variance_at(1.5, k), w_late, 1e-12);
  EXPECT_NEAR(surface.total_variance_at(1.0, k), 0.5 * (w_early + w_late), 1e-12);
  EXPECT_NEAR(surface.total_variance_at(0.75, k), w_early + 0.25 * (w_late - w_early),
              1e-12);
}

// Interpolating volatility instead of total variance could produce a dip
// between two arbitrage-free slices. This asserts the property that rules
// that out.
TEST(Surface, InterpolationNeverDecreasesTotalVariance) {
  const VolSurface surface = fit_surface(generate_chain(ChainConfig{}));
  ASSERT_TRUE(surface.ok());

  for (const double k : {-0.2, -0.05, 0.0, 0.05, 0.2}) {
    double previous = 0.0;
    for (double time = 0.02; time <= 2.0; time += 0.01) {
      const double current = surface.total_variance_at(time, k);
      EXPECT_GE(current, previous - 1e-12) << "k=" << k << " T=" << time;
      previous = current;
    }
  }
}

TEST(Surface, ExtrapolationHoldsVolatilityFlat) {
  const SviParameters p{.a = 0.04, .b = 0.10, .rho = -0.3, .m = 0.0, .sigma = 0.2};

  VolSurface surface{};
  surface.slices = {MakeSlice(0.5, p), MakeSlice(1.5, p)};
  surface.status = SurfaceStatus::Success;

  const double k = 0.05;
  const double short_vol = surface.volatility_at(0.5, k);
  EXPECT_NEAR(surface.volatility_at(0.1, k), short_vol, 1e-12);

  const double long_vol = surface.volatility_at(1.5, k);
  EXPECT_NEAR(surface.volatility_at(5.0, k), long_vol, 1e-12);
}

TEST(Surface, EmptyChainIsReportedRatherThanCrashing) {
  const OptionChain empty{};
  const VolSurface surface = fit_surface(empty);
  EXPECT_EQ(surface.status, SurfaceStatus::NoUsableExpiries);
  EXPECT_FALSE(surface.ok());
  EXPECT_DOUBLE_EQ(surface.volatility_at(1.0, 0.0), 0.0);
}

}  // namespace
}  // namespace skewdesk
