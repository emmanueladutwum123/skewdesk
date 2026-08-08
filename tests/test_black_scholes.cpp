#include "skewdesk/black_scholes.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

// d1 lands on exactly 0.35 and d2 on exactly 0.15 for this parameter set,
// which makes it hand-checkable against a normal table rather than only
// against another implementation.
BlackScholesInputs BaseCase() {
  return BlackScholesInputs{.spot = 100.0,
                            .strike = 100.0,
                            .rate = 0.05,
                            .dividend = 0.0,
                            .volatility = 0.20,
                            .time = 1.0};
}

using Field = double BlackScholesInputs::*;

double PriceBumped(BlackScholesInputs in, Field field, double delta, OptionType type) {
  in.*field += delta;
  return price(in, type);
}

// Central difference: error is O(h^2) rather than the O(h) of a forward
// difference, which is what makes tolerances this tight achievable.
double FirstDerivative(const BlackScholesInputs& in, Field field, double h, OptionType type) {
  return (PriceBumped(in, field, h, type) - PriceBumped(in, field, -h, type)) / (2.0 * h);
}

double SecondDerivative(const BlackScholesInputs& in, Field field, double h, OptionType type) {
  const double up = PriceBumped(in, field, h, type);
  const double mid = price(in, type);
  const double down = PriceBumped(in, field, -h, type);
  return (up - 2.0 * mid + down) / (h * h);
}

std::vector<BlackScholesInputs> Grid() {
  std::vector<BlackScholesInputs> cases;
  for (const double strike : {70.0, 90.0, 100.0, 110.0, 140.0}) {
    for (const double vol : {0.10, 0.25, 0.60}) {
      for (const double time : {0.08, 1.0, 3.0}) {
        for (const double dividend : {0.0, 0.03}) {
          cases.push_back(BlackScholesInputs{.spot = 100.0,
                                             .strike = strike,
                                             .rate = 0.04,
                                             .dividend = dividend,
                                             .volatility = vol,
                                             .time = time});
        }
      }
    }
  }
  return cases;
}

TEST(BlackScholes, MatchesHandComputedReference) {
  const auto in = BaseCase();
  EXPECT_NEAR(price(in, OptionType::Call), 10.450583572185572, 1e-12);
  EXPECT_NEAR(price(in, OptionType::Put), 5.573526022256967, 1e-12);
}

TEST(BlackScholes, GreeksMatchHandComputedReference) {
  const Greeks g = greeks(BaseCase(), OptionType::Call);
  EXPECT_NEAR(g.delta, 0.636830651175619, 1e-12);
  EXPECT_NEAR(g.gamma, 0.018762017345847, 1e-12);
  EXPECT_NEAR(g.vega, 37.524034691693785, 1e-12);
  EXPECT_NEAR(g.theta, -6.414027546438197, 1e-12);
  EXPECT_NEAR(g.rho, 53.232481545376366, 1e-12);
}

// The single most valuable invariant in an options library: it ties calls and
// puts together with no reference to the model at all, so a sign error or a
// misplaced discount factor in either branch breaks it immediately.
TEST(BlackScholes, PutCallParityHoldsAcrossGrid) {
  for (const auto& in : Grid()) {
    const double call = price(in, OptionType::Call);
    const double put = price(in, OptionType::Put);
    const double expected =
        discount_factor(in) * (forward_price(in) - in.strike);
    EXPECT_NEAR(call - put, expected, 1e-10)
        << "K=" << in.strike << " vol=" << in.volatility << " T=" << in.time
        << " q=" << in.dividend;
  }
}

TEST(BlackScholes, GreeksMatchFiniteDifferencesAcrossGrid) {
  for (const auto& in : Grid()) {
    for (const OptionType type : {OptionType::Call, OptionType::Put}) {
      const Greeks g = greeks(in, type);

      EXPECT_NEAR(g.delta,
                  FirstDerivative(in, &BlackScholesInputs::spot, 1e-4, type), 1e-7)
          << "delta K=" << in.strike << " vol=" << in.volatility << " T=" << in.time;

      EXPECT_NEAR(g.gamma,
                  SecondDerivative(in, &BlackScholesInputs::spot, 1e-2, type), 1e-6)
          << "gamma K=" << in.strike << " vol=" << in.volatility << " T=" << in.time;

      EXPECT_NEAR(g.vega,
                  FirstDerivative(in, &BlackScholesInputs::volatility, 1e-5, type), 1e-5)
          << "vega K=" << in.strike << " vol=" << in.volatility << " T=" << in.time;

      EXPECT_NEAR(g.rho,
                  FirstDerivative(in, &BlackScholesInputs::rate, 1e-5, type), 1e-5)
          << "rho K=" << in.strike << " vol=" << in.volatility << " T=" << in.time;

      // Theta is the derivative with respect to *calendar* time, and time to
      // expiry shrinks as calendar time advances -- hence the sign flip. A
      // library that gets this backwards still passes every other test here.
      EXPECT_NEAR(g.theta,
                  -FirstDerivative(in, &BlackScholesInputs::time, 1e-5, type), 1e-4)
          << "theta K=" << in.strike << " vol=" << in.volatility << " T=" << in.time;
    }
  }
}

TEST(BlackScholes, GammaAndVegaAreIdenticalForCallAndPut) {
  for (const auto& in : Grid()) {
    const Greeks call = greeks(in, OptionType::Call);
    const Greeks put = greeks(in, OptionType::Put);
    EXPECT_NEAR(call.gamma, put.gamma, 1e-15);
    EXPECT_NEAR(call.vega, put.vega, 1e-12);
  }
}

// Differentiating put-call parity once with respect to spot: the difference
// between call and put delta must be exactly the dividend discount factor.
TEST(BlackScholes, DeltaSpreadEqualsDividendDiscountFactor) {
  for (const auto& in : Grid()) {
    const double call = greeks(in, OptionType::Call).delta;
    const double put = greeks(in, OptionType::Put).delta;
    EXPECT_NEAR(call - put, std::exp(-in.dividend * in.time), 1e-12);
  }
}

TEST(BlackScholes, PriceIsMonotonicIncreasingInVolatility) {
  for (const auto& in : Grid()) {
    for (const OptionType type : {OptionType::Call, OptionType::Put}) {
      BlackScholesInputs lower = in;
      BlackScholesInputs higher = in;
      lower.volatility = in.volatility * 0.5;
      higher.volatility = in.volatility * 2.0;
      EXPECT_LT(price(lower, type), price(higher, type))
          << "K=" << in.strike << " T=" << in.time;
    }
  }
}

TEST(BlackScholes, AtExpiryPriceCollapsesToIntrinsic) {
  BlackScholesInputs in = BaseCase();
  in.time = 0.0;

  in.spot = 120.0;
  EXPECT_DOUBLE_EQ(price(in, OptionType::Call), 20.0);
  EXPECT_DOUBLE_EQ(price(in, OptionType::Put), 0.0);

  in.spot = 80.0;
  EXPECT_DOUBLE_EQ(price(in, OptionType::Call), 0.0);
  EXPECT_DOUBLE_EQ(price(in, OptionType::Put), 20.0);
}

TEST(BlackScholes, ZeroVolatilityCollapsesToDiscountedIntrinsicOnForward) {
  BlackScholesInputs in = BaseCase();
  in.volatility = 0.0;

  const double forward = forward_price(in);
  const double df = discount_factor(in);
  EXPECT_DOUBLE_EQ(price(in, OptionType::Call), df * (forward - in.strike));
  EXPECT_DOUBLE_EQ(price(in, OptionType::Put), 0.0);
}

TEST(BlackScholes, DeterministicGreeksAreAStepFunction) {
  BlackScholesInputs in = BaseCase();
  in.time = 0.0;

  in.spot = 120.0;
  Greeks g = greeks(in, OptionType::Call);
  EXPECT_DOUBLE_EQ(g.delta, 1.0);
  EXPECT_DOUBLE_EQ(g.gamma, 0.0);
  EXPECT_DOUBLE_EQ(g.vega, 0.0);

  in.spot = 80.0;
  g = greeks(in, OptionType::Call);
  EXPECT_DOUBLE_EQ(g.delta, 0.0);

  g = greeks(in, OptionType::Put);
  EXPECT_DOUBLE_EQ(g.delta, -1.0);
}

TEST(BlackScholes, DegenerateSpotAndStrikeAreHandledAsLimits) {
  BlackScholesInputs in = BaseCase();

  in.spot = 0.0;
  EXPECT_DOUBLE_EQ(price(in, OptionType::Call), 0.0);
  EXPECT_DOUBLE_EQ(price(in, OptionType::Put), discount_factor(in) * in.strike);

  in = BaseCase();
  in.strike = 0.0;
  EXPECT_DOUBLE_EQ(price(in, OptionType::Call),
                   discount_factor(in) * forward_price(in));
  EXPECT_DOUBLE_EQ(price(in, OptionType::Put), 0.0);
}

TEST(BlackScholes, DividendYieldReducesCallsAndRaisesPuts) {
  BlackScholesInputs none = BaseCase();
  BlackScholesInputs paying = BaseCase();
  paying.dividend = 0.04;

  EXPECT_LT(price(paying, OptionType::Call), price(none, OptionType::Call));
  EXPECT_GT(price(paying, OptionType::Put), price(none, OptionType::Put));
}

TEST(BlackScholes, DeepOutOfTheMoneyOptionsArePositiveButNegligible) {
  BlackScholesInputs in = BaseCase();
  in.strike = 500.0;

  const double call = price(in, OptionType::Call);
  EXPECT_GT(call, 0.0);
  EXPECT_LT(call, 1e-4);
}

// The erfc formulation exists specifically to keep this accurate; the naive
// 0.5 * (1 + erf(x / sqrt(2))) returns a flat zero out here, which would make
// every far-wing option in a chain price at exactly intrinsic.
TEST(BlackScholes, NormalCdfStaysAccurateDeepInTheLeftTail) {
  EXPECT_GT(norm_cdf(-10.0), 0.0);
  EXPECT_NEAR(norm_cdf(-10.0), 7.619853024160525e-24, 1e-30);
  EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-15);
  EXPECT_NEAR(norm_cdf(-1.0) + norm_cdf(1.0), 1.0, 1e-15);
}

}  // namespace
}  // namespace skewdesk
