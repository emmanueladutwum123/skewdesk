#include "skewdesk/implied_volatility.hpp"
#include "skewdesk/parity.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

// Builds a same-expiry chain that is parity-consistent by construction, so the
// regression has an exact answer to find.
std::vector<ParityQuote> SyntheticChain(double forward, double discount,
                                        const std::vector<double>& strikes,
                                        double volatility, double time) {
  std::vector<ParityQuote> chain;
  chain.reserve(strikes.size());
  for (const double strike : strikes) {
    const ForwardInputs in{.forward = forward,
                           .strike = strike,
                           .discount_factor = discount,
                           .volatility = volatility,
                           .time = time};
    chain.push_back(ParityQuote{.strike = strike,
                                .call_price = price(in, OptionType::Call),
                                .put_price = price(in, OptionType::Put)});
  }
  return chain;
}

std::vector<double> Strikes() {
  return {3800.0, 4000.0, 4200.0, 4400.0, 4500.0, 4600.0, 4800.0, 5000.0, 5200.0};
}

TEST(Parity, RecoversForwardAndDiscountFromACleanChain) {
  const double forward = 4523.75;
  const double discount = std::exp(-0.042 * 0.35);

  const std::vector<ParityQuote> chain =
      SyntheticChain(forward, discount, Strikes(), 0.19, 0.35);
  const ParityFit fit = fit_forward_and_discount(chain);

  ASSERT_TRUE(fit.ok());
  EXPECT_NEAR(fit.forward, forward, 1e-9);
  EXPECT_NEAR(fit.discount_factor, discount, 1e-12);
  EXPECT_NEAR(fit.r_squared, 1.0, 1e-12);
}

// The recovered forward must not depend on the volatility used to build the
// chain: parity is model-free, so a smile in the input cannot bias the fit.
TEST(Parity, RecoveredForwardIsIndependentOfTheVolatilityUsed) {
  const double forward = 4523.75;
  const double discount = 0.9854;

  for (const double vol : {0.05, 0.20, 0.80}) {
    const ParityFit fit =
        fit_forward_and_discount(SyntheticChain(forward, discount, Strikes(), vol, 0.35));
    ASSERT_TRUE(fit.ok()) << "vol=" << vol;
    EXPECT_NEAR(fit.forward, forward, 1e-8) << "vol=" << vol;
    EXPECT_NEAR(fit.discount_factor, discount, 1e-12) << "vol=" << vol;
  }
}

TEST(Parity, AveragesAwayQuoteNoiseAndReportsDegradedFitQuality) {
  const double forward = 4523.75;
  const double discount = 0.9854;

  std::vector<ParityQuote> chain =
      SyntheticChain(forward, discount, Strikes(), 0.19, 0.35);

  // A deterministic alternating perturbation standing in for bid/ask noise:
  // zero-mean across the chain, so the fit should still centre on the truth
  // while the residuals show up in R-squared.
  double sign = 1.0;
  for (ParityQuote& q : chain) {
    q.call_price += sign * 0.35;
    sign = -sign;
  }

  const ParityFit fit = fit_forward_and_discount(chain);
  ASSERT_TRUE(fit.ok());
  EXPECT_NEAR(fit.forward, forward, 1.0);
  EXPECT_LT(fit.r_squared, 1.0);
  EXPECT_GT(fit.r_squared, 0.99);
}

TEST(Parity, RejectsChainsThatCannotDetermineALine) {
  const std::vector<ParityQuote> one = {{4500.0, 120.0, 100.0}};
  EXPECT_EQ(fit_forward_and_discount(one).status, ParityStatus::InsufficientQuotes);

  const std::vector<ParityQuote> same_strike = {
      {4500.0, 120.0, 100.0}, {4500.0, 121.0, 101.0}, {4500.0, 119.0, 99.0}};
  EXPECT_EQ(fit_forward_and_discount(same_strike).status, ParityStatus::DegenerateStrikes);
}

// Parity forces the slope of (call - put) against strike to be negative. A
// non-negative slope means the input is not a coherent same-expiry chain --
// mismatched expiries, or calls and puts that were never actually paired.
TEST(Parity, RejectsIncoherentChainsInsteadOfReturningANegativeDiscount) {
  const std::vector<ParityQuote> ascending = {
      {4000.0, 10.0, 100.0}, {4500.0, 60.0, 100.0}, {5000.0, 110.0, 100.0}};
  EXPECT_EQ(fit_forward_and_discount(ascending).status,
            ParityStatus::NonPositiveDiscountFactor);
}

// The full M2 pipeline: start from a known forward, discount, and volatility
// smile; build a chain; recover the forward and discount with no knowledge of
// the rate or dividend yield; then back out the smile from the recovered
// values. Getting the original smile back means every piece agrees.
TEST(Parity, EndToEndRecoversTheSmileWithoutAnyQuotedRate) {
  const double true_forward = 4487.5;
  const double true_discount = std::exp(-0.0385 * 0.4);
  const double time = 0.4;
  const std::vector<double> strikes = Strikes();

  // A downward-sloping skew, as equity index options actually exhibit:
  // downside strikes carry higher implied volatility than upside ones.
  const auto smile_vol = [&](double strike) {
    return 0.22 - 0.12 * (strike / true_forward - 1.0);
  };

  std::vector<ParityQuote> chain;
  for (const double strike : strikes) {
    const ForwardInputs in{.forward = true_forward,
                           .strike = strike,
                           .discount_factor = true_discount,
                           .volatility = smile_vol(strike),
                           .time = time};
    chain.push_back(ParityQuote{.strike = strike,
                                .call_price = price(in, OptionType::Call),
                                .put_price = price(in, OptionType::Put)});
  }

  const ParityFit fit = fit_forward_and_discount(chain);
  ASSERT_TRUE(fit.ok());
  EXPECT_NEAR(fit.forward, true_forward, 1e-6);
  EXPECT_NEAR(fit.discount_factor, true_discount, 1e-9);

  for (const ParityQuote& quote : chain) {
    const ImpliedVolQuery q{.forward = fit.forward,
                            .strike = quote.strike,
                            .discount_factor = fit.discount_factor,
                            .time = time};

    const ImpliedVolResult from_call =
        implied_volatility(quote.call_price, q, OptionType::Call);
    const ImpliedVolResult from_put =
        implied_volatility(quote.put_price, q, OptionType::Put);

    ASSERT_TRUE(from_call.ok()) << "K=" << quote.strike;
    ASSERT_TRUE(from_put.ok()) << "K=" << quote.strike;

    EXPECT_NEAR(from_call.volatility, smile_vol(quote.strike), 1e-7) << "K=" << quote.strike;
    EXPECT_NEAR(from_put.volatility, smile_vol(quote.strike), 1e-7) << "K=" << quote.strike;
  }
}

}  // namespace
}  // namespace skewdesk
