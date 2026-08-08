#include "skewdesk/svi.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

SviParameters Benign() {
  return SviParameters{.a = 0.04, .b = 0.10, .rho = -0.40, .m = 0.0, .sigma = 0.20};
}

// Confirmed to break Durrleman's condition: a very sharp vertex with strongly
// asymmetric wings bends total variance faster than a probability density can
// absorb.
SviParameters ButterflyViolating() {
  return SviParameters{.a = 0.02, .b = 0.60, .rho = -0.90, .m = 0.0, .sigma = 0.02};
}

std::vector<SviObservation> SampleSlice(const SviParameters& p, int count = 21,
                                        double k_low = -0.35, double k_high = 0.35) {
  std::vector<SviObservation> observations;
  for (int i = 0; i < count; ++i) {
    const double position = static_cast<double>(i) / static_cast<double>(count - 1);
    const double k = k_low + position * (k_high - k_low);
    observations.push_back(
        SviObservation{.log_moneyness = k, .total_variance = total_variance(p, k)});
  }
  return observations;
}

TEST(Svi, TotalVarianceMatchesTheHyperbolaByHand) {
  const SviParameters p = Benign();
  // At k = m the sqrt term is exactly sigma and the rho term vanishes.
  EXPECT_NEAR(total_variance(p, p.m), p.a + p.b * p.sigma, 1e-15);
  EXPECT_NEAR(total_variance(p, 0.3),
              0.04 + 0.10 * (-0.40 * 0.3 + std::hypot(0.3, 0.20)), 1e-15);
}

// Same technique that validated the M1 greeks: check every analytic derivative
// against a numerical one, so a transcription slip cannot survive.
TEST(Svi, DerivativesMatchFiniteDifferences) {
  for (const SviParameters& p : {Benign(), ButterflyViolating()}) {
    for (const double k : {-0.5, -0.2, -0.01, 0.0, 0.01, 0.2, 0.5}) {
      const double h = 1e-5;
      const double slope =
          (total_variance(p, k + h) - total_variance(p, k - h)) / (2.0 * h);
      const double curvature =
          (total_variance(p, k + h) - 2.0 * total_variance(p, k) +
           total_variance(p, k - h)) /
          (h * h);

      EXPECT_NEAR(total_variance_slope(p, k), slope, 1e-6) << "k=" << k;
      EXPECT_NEAR(total_variance_curvature(p, k), curvature, 1e-3) << "k=" << k;
    }
  }
}

// SVI is a hyperbola, so far from the vertex it becomes two straight lines.
// Those asymptotic slopes are what the wing parameters actually mean.
TEST(Svi, WingSlopesApproachTheirAsymptotes) {
  const SviParameters p = Benign();
  EXPECT_NEAR(total_variance_slope(p, 5000.0), p.b * (p.rho + 1.0), 1e-6);
  EXPECT_NEAR(total_variance_slope(p, -5000.0), p.b * (p.rho - 1.0), 1e-6);
}

TEST(Svi, MinimumTotalVarianceIsAttainedWhereTheoryPredicts) {
  const SviParameters p = Benign();
  const double k_star = p.m - p.rho * p.sigma / std::sqrt(1.0 - p.rho * p.rho);

  EXPECT_NEAR(total_variance(p, k_star), minimum_total_variance(p), 1e-14);
  EXPECT_NEAR(total_variance_slope(p, k_star), 0.0, 1e-12);
  for (const double k : {-1.0, -0.3, 0.0, 0.3, 1.0}) {
    EXPECT_GE(total_variance(p, k), minimum_total_variance(p) - 1e-14);
  }
}

TEST(Svi, DurrlemanIsPositiveOnABenignSlice) {
  const ButterflyCheck check = check_butterfly(Benign(), -1.0, 1.0);
  EXPECT_TRUE(check.free_of_arbitrage);
  EXPECT_GT(check.worst_value, 0.0);
}

TEST(Svi, DurrlemanDetectsAViolatingSlice) {
  const ButterflyCheck check = check_butterfly(ButterflyViolating(), -1.0, 1.0);
  EXPECT_FALSE(check.free_of_arbitrage);
  EXPECT_LT(check.worst_value, 0.0);
  EXPECT_GT(check.worst_log_moneyness, -1.0);
  EXPECT_LT(check.worst_log_moneyness, 1.0);
}

TEST(Svi, FitRecoversKnownParametersFromExactObservations) {
  const SviParameters truth = Benign();
  const std::vector<SviObservation> observations = SampleSlice(truth);

  const SviFit fit = fit_svi(observations, 1.0);
  ASSERT_TRUE(fit.ok()) << "status=" << static_cast<int>(fit.status);
  EXPECT_LT(fit.rmse, 1e-8);

  // The parameters themselves are only weakly identified from a finite strike
  // band -- several combinations trace nearly the same curve there -- so the
  // curve is what gets asserted, not the five numbers.
  for (const double k : {-0.35, -0.15, 0.0, 0.15, 0.35}) {
    EXPECT_NEAR(total_variance(fit.parameters, k), total_variance(truth, k), 1e-7)
        << "k=" << k;
  }
}

TEST(Svi, FitReportsButterflyArbitrageRatherThanHidingIt) {
  const std::vector<SviObservation> observations =
      SampleSlice(ButterflyViolating(), 31, -0.2, 0.2);

  const SviFit fit = fit_svi(observations, 1.0);
  EXPECT_EQ(fit.status, SviFitStatus::ButterflyArbitrage);
  EXPECT_FALSE(fit.ok());
  EXPECT_FALSE(fit.butterfly.free_of_arbitrage);
  // Parameters are still returned so the violation can be inspected and
  // reported rather than merely refused.
  EXPECT_GT(fit.parameters.b, 0.0);
}

TEST(Svi, FitRejectsInputsItCannotDetermine) {
  const std::vector<SviObservation> too_few = SampleSlice(Benign(), 4);
  EXPECT_EQ(fit_svi(too_few, 1.0).status, SviFitStatus::InsufficientObservations);

  std::vector<SviObservation> negative = SampleSlice(Benign());
  negative[3].total_variance = -0.01;
  EXPECT_EQ(fit_svi(negative, 1.0).status, SviFitStatus::DegenerateObservations);

  std::vector<SviObservation> single_strike = SampleSlice(Benign());
  for (SviObservation& observation : single_strike) {
    observation.log_moneyness = 0.1;
  }
  EXPECT_EQ(fit_svi(single_strike, 1.0).status, SviFitStatus::DegenerateObservations);
}

TEST(Svi, FittedSliceAlwaysHasStrictlyPositiveTotalVariance) {
  const SviFit fit = fit_svi(SampleSlice(Benign()), 1.0);
  ASSERT_TRUE(fit.ok());
  EXPECT_GT(minimum_total_variance(fit.parameters), 0.0);
}

TEST(Svi, VolatilityIsTheSquareRootOfTotalVarianceOverMaturity) {
  const SviParameters p = Benign();
  const double time = 0.75;
  for (const double k : {-0.2, 0.0, 0.2}) {
    EXPECT_NEAR(svi_volatility(p, k, time), std::sqrt(total_variance(p, k) / time), 1e-14);
  }
  EXPECT_DOUBLE_EQ(svi_volatility(p, 0.0, 0.0), 0.0);
}

TEST(Svi, FitIsReproducible) {
  const std::vector<SviObservation> observations = SampleSlice(Benign());
  const SviFit first = fit_svi(observations, 1.0);
  const SviFit second = fit_svi(observations, 1.0);

  EXPECT_DOUBLE_EQ(first.parameters.a, second.parameters.a);
  EXPECT_DOUBLE_EQ(first.parameters.b, second.parameters.b);
  EXPECT_DOUBLE_EQ(first.parameters.rho, second.parameters.rho);
  EXPECT_DOUBLE_EQ(first.parameters.m, second.parameters.m);
  EXPECT_DOUBLE_EQ(first.parameters.sigma, second.parameters.sigma);
}

}  // namespace
}  // namespace skewdesk
