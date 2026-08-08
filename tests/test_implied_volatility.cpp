#include "skewdesk/implied_volatility.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

constexpr double kForward = 4500.0;

ImpliedVolQuery Query(double strike, double time) {
  return ImpliedVolQuery{.forward = kForward,
                         .strike = strike,
                         .discount_factor = std::exp(-0.042 * time),
                         .time = time};
}

// A realistic index chain's moneyness band, from the deep put wing to the deep
// call wing.
std::vector<double> Strikes() {
  return {3000.0, 3600.0, 4050.0, 4275.0, 4500.0, 4725.0, 4950.0, 5400.0, 6300.0};
}

std::vector<double> Tenors() { return {0.02, 0.25, 1.0, 2.0}; }

std::vector<double> Vols() { return {0.08, 0.15, 0.30, 0.65}; }

// Inverts the out-of-the-money side at every strike, which is what a desk
// does and what out_of_the_money_type() exists to express. The in-the-money
// side is deliberately not exercised here -- see
// DeepInTheMoneyQuotesCarryNoVolatilityInformation for why it cannot be.
TEST(ImpliedVolatility, RoundTripRecoversInputVolatilityOnTheOutOfTheMoneySide) {
  for (const double strike : Strikes()) {
    for (const double time : Tenors()) {
      for (const double vol : Vols()) {
        const ImpliedVolQuery q = Query(strike, time);
        const OptionType type = out_of_the_money_type(kForward, strike);

        const double target = price(with_volatility(q, vol), type);
        const ImpliedVolResult r = implied_volatility(target, q, type);

        ASSERT_TRUE(r.ok()) << "status=" << static_cast<int>(r.status)
                            << " K=" << strike << " T=" << time << " vol=" << vol;
        EXPECT_NEAR(r.volatility, vol, 1e-9)
            << "K=" << strike << " T=" << time << " type="
            << (type == OptionType::Call ? "call" : "put");
      }
    }
  }
}

// The safeguard's reason for existing. At this strike vega is 1.5e-15 -- an
// unguarded Newton step divides by it and diverges immediately, while the
// price itself, at 4e-18, is still a perfectly ordinary double. The bracket is
// what keeps the iteration alive, and the rejection conditions are written so
// that a vanishing vega forces bisection with no special case.
TEST(ImpliedVolatility, ConvergesInTheDeepWingsWhereVegaVanishes) {
  const ImpliedVolQuery q = Query(7000.0, 0.05);
  const double vol = 0.22;
  const double target = price(with_volatility(q, vol), OptionType::Call);

  ASSERT_GT(target, 0.0);
  ASSERT_LT(forward_vega(with_volatility(q, vol)), 1e-12) << "vega is not actually small";

  const ImpliedVolResult r = implied_volatility(target, q, OptionType::Call);
  ASSERT_TRUE(r.ok()) << "status=" << static_cast<int>(r.status);
  EXPECT_NEAR(r.volatility, vol, 1e-9);
}

// Parity ties the two together: where both sides are invertible, a call and a
// put at one strike priced consistently must imply the same volatility. The
// band is kept near the forward and away from the shortest tenor because that
// is precisely the region where the in-the-money side still carries enough
// time value to be inverted at all.
TEST(ImpliedVolatility, CallAndPutAgreeWhereBothSidesAreInvertible) {
  for (const double strike : {4275.0, 4400.0, 4500.0, 4600.0, 4725.0}) {
    for (const double time : {0.25, 1.0, 2.0}) {
      const ImpliedVolQuery q = Query(strike, time);
      const double vol = 0.21;

      const ImpliedVolResult from_call =
          implied_volatility(price(with_volatility(q, vol), OptionType::Call), q,
                             OptionType::Call);
      const ImpliedVolResult from_put =
          implied_volatility(price(with_volatility(q, vol), OptionType::Put), q,
                             OptionType::Put);

      ASSERT_TRUE(from_call.ok()) << "K=" << strike << " T=" << time;
      ASSERT_TRUE(from_put.ok()) << "K=" << strike << " T=" << time;
      EXPECT_NEAR(from_call.volatility, from_put.volatility, 1e-9)
          << "K=" << strike << " T=" << time;
    }
  }
}

// Pins the limitation itself, so that it stays documented behaviour rather
// than something rediscovered later. Three very different volatilities produce
// byte-identical prices for this contract: the time value has fallen below the
// resolution of a double sitting next to an intrinsic value of ~4500. Any
// number the solver returned here would be fiction.
TEST(ImpliedVolatility, DeepInTheMoneyQuotesCarryNoVolatilityInformation) {
  const ImpliedVolQuery q = Query(3000.0, 0.02);

  const double at_low = price(with_volatility(q, 0.08), OptionType::Call);
  const double at_high = price(with_volatility(q, 0.30), OptionType::Call);
  ASSERT_DOUBLE_EQ(at_low, at_high) << "premise broken: these prices now differ";

  const ImpliedVolResult r = implied_volatility(at_low, q, OptionType::Call);
  EXPECT_EQ(r.status, ImpliedVolStatus::IllConditioned);
  EXPECT_FALSE(r.ok());

  // The out-of-the-money side of the same strike inverts cleanly, which is the
  // documented remedy.
  const OptionType otm = out_of_the_money_type(kForward, 3000.0);
  EXPECT_EQ(otm, OptionType::Put);
  const ImpliedVolResult from_otm =
      implied_volatility(price(with_volatility(q, 0.30), otm), q, otm);
  ASSERT_TRUE(from_otm.ok());
  EXPECT_NEAR(from_otm.volatility, 0.30, 1e-9);
}

TEST(ImpliedVolatility, OutOfTheMoneySideIsChosenRelativeToTheForward) {
  EXPECT_EQ(out_of_the_money_type(4500.0, 5000.0), OptionType::Call);
  EXPECT_EQ(out_of_the_money_type(4500.0, 4000.0), OptionType::Put);
  EXPECT_EQ(out_of_the_money_type(4500.0, 4500.0), OptionType::Call);
}

TEST(ImpliedVolatility, QuoteBelowIntrinsicIsRejectedRatherThanApproximated) {
  const ImpliedVolQuery q = Query(3600.0, 0.5);
  const double floor_price = q.discount_factor * (q.forward - q.strike);

  const ImpliedVolResult r =
      implied_volatility(floor_price * 0.99, q, OptionType::Call);
  EXPECT_EQ(r.status, ImpliedVolStatus::PriceBelowIntrinsic);
  EXPECT_FALSE(r.ok());
}

TEST(ImpliedVolatility, QuoteAboveUpperBoundIsRejected) {
  const ImpliedVolQuery q = Query(4500.0, 0.5);

  const ImpliedVolResult call =
      implied_volatility(q.discount_factor * q.forward * 1.01, q, OptionType::Call);
  EXPECT_EQ(call.status, ImpliedVolStatus::PriceAboveUpperBound);

  const ImpliedVolResult put =
      implied_volatility(q.discount_factor * q.strike * 1.01, q, OptionType::Put);
  EXPECT_EQ(put.status, ImpliedVolStatus::PriceAboveUpperBound);
}

// A quote sitting exactly on intrinsic has zero volatility as its only exact
// solution, but every volatility within a rounding error of zero reproduces
// the same double -- so the honest answer is that the quote does not pin one
// down, not that it implies zero.
TEST(ImpliedVolatility, QuoteExactlyAtIntrinsicIsReportedAsIllConditioned) {
  const ImpliedVolQuery q = Query(3600.0, 0.5);
  const double floor_price = q.discount_factor * (q.forward - q.strike);

  const ImpliedVolResult r = implied_volatility(floor_price, q, OptionType::Call);
  EXPECT_EQ(r.status, ImpliedVolStatus::IllConditioned);
}

// Out of the money the intrinsic value is zero, so the conditioning threshold
// collapses to "is the quote strictly positive" -- and a legitimately tiny
// far-wing price stays invertible rather than being swallowed by a tolerance.
TEST(ImpliedVolatility, TinyOutOfTheMoneyQuotesRemainInvertible) {
  const ImpliedVolQuery q = Query(6000.0, 0.05);
  const double target = price(with_volatility(q, 0.22), OptionType::Call);

  ASSERT_LT(target, 1e-6) << "premise broken: this quote is not tiny";
  const ImpliedVolResult r = implied_volatility(target, q, OptionType::Call);
  ASSERT_TRUE(r.ok()) << "status=" << static_cast<int>(r.status);
  EXPECT_NEAR(r.volatility, 0.22, 1e-9);

  const ImpliedVolResult worthless = implied_volatility(0.0, q, OptionType::Call);
  EXPECT_EQ(worthless.status, ImpliedVolStatus::IllConditioned);
}

TEST(ImpliedVolatility, MalformedContractsAreRejected) {
  for (ImpliedVolQuery q : {ImpliedVolQuery{kForward, 4500.0, 0.98, 0.0},
                            ImpliedVolQuery{0.0, 4500.0, 0.98, 0.5},
                            ImpliedVolQuery{kForward, 0.0, 0.98, 0.5},
                            ImpliedVolQuery{kForward, 4500.0, 0.0, 0.5}}) {
    const ImpliedVolResult r = implied_volatility(100.0, q, OptionType::Call);
    EXPECT_EQ(r.status, ImpliedVolStatus::DegenerateContract);
  }
}

// Safeguarded Newton keeps quadratic convergence wherever Newton is well
// behaved; if this regressed to pure bisection the count would jump toward the
// ~40 iterations needed to bisect down to the tolerance.
TEST(ImpliedVolatility, ConvergesInFewIterationsNearTheMoney) {
  const ImpliedVolQuery q = Query(4500.0, 0.25);
  const double target = price(with_volatility(q, 0.18), OptionType::Call);

  const ImpliedVolResult r = implied_volatility(target, q, OptionType::Call);
  ASSERT_TRUE(r.ok());
  EXPECT_LE(r.iterations, 12) << "took " << r.iterations << " iterations";
}

TEST(ImpliedVolatility, ForwardAndSpotParameterizationsAgree) {
  const BlackScholesInputs spot_in{.spot = 100.0,
                                   .strike = 110.0,
                                   .rate = 0.045,
                                   .dividend = 0.017,
                                   .volatility = 0.23,
                                   .time = 0.75};
  const ForwardInputs fwd_in = to_forward(spot_in);

  for (const OptionType type : {OptionType::Call, OptionType::Put}) {
    EXPECT_DOUBLE_EQ(price(spot_in, type), price(fwd_in, type));
  }
  EXPECT_NEAR(forward_vega(fwd_in), greeks(spot_in, OptionType::Call).vega, 1e-10);
}

}  // namespace
}  // namespace skewdesk
