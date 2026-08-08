#include "skewdesk/chain.hpp"
#include "skewdesk/implied_volatility.hpp"
#include "skewdesk/parity.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <map>
#include <vector>

namespace skewdesk {
namespace {

std::vector<ContractQuote> QuotesOfType(const ExpirySlice& slice, OptionType type) {
  std::vector<ContractQuote> selected;
  for (const ContractQuote& quote : slice.quotes) {
    if (quote.type == type) {
      selected.push_back(quote);
    }
  }
  return selected;
}

// Pairs the two sides at each strike into the form the parity regression eats.
std::vector<ParityQuote> ToParityQuotes(const ExpirySlice& slice) {
  std::map<double, ParityQuote> paired;
  for (const ContractQuote& quote : slice.quotes) {
    ParityQuote& entry = paired[quote.strike];
    entry.strike = quote.strike;
    if (quote.type == OptionType::Call) {
      entry.call_price = quote.mid;
    } else {
      entry.put_price = quote.mid;
    }
  }

  std::vector<ParityQuote> result;
  result.reserve(paired.size());
  for (const auto& [strike, quote] : paired) {
    result.push_back(quote);
  }
  return result;
}

TEST(Chain, IsReproducibleFromItsSeed) {
  ChainConfig config{};
  config.quote_noise_vol_points = 0.004;

  const OptionChain first = generate_chain(config);
  const OptionChain second = generate_chain(config);

  ASSERT_EQ(first.expiries.size(), second.expiries.size());
  for (std::size_t e = 0; e < first.expiries.size(); ++e) {
    ASSERT_EQ(first.expiries[e].quotes.size(), second.expiries[e].quotes.size());
    for (std::size_t q = 0; q < first.expiries[e].quotes.size(); ++q) {
      EXPECT_DOUBLE_EQ(first.expiries[e].quotes[q].mid,
                       second.expiries[e].quotes[q].mid);
    }
  }

  config.seed += 1;
  const OptionChain different = generate_chain(config);
  bool any_difference = false;
  for (std::size_t q = 0; q < first.expiries[0].quotes.size(); ++q) {
    if (first.expiries[0].quotes[q].mid != different.expiries[0].quotes[q].mid) {
      any_difference = true;
      break;
    }
  }
  EXPECT_TRUE(any_difference) << "a different seed produced an identical chain";
}

// The defining feature of an equity index surface: downside strikes carry
// higher implied volatility than upside ones.
TEST(Chain, VolatilitySlopesDownwardAcrossStrikes) {
  const SkewParameters skew{};
  for (const double time : {0.08, 0.25, 1.0, 2.0}) {
    double previous = skew_volatility(skew, -0.15, time);
    for (const double k : {-0.10, -0.05, 0.0, 0.05}) {
      const double current = skew_volatility(skew, k, time);
      EXPECT_LT(current, previous) << "k=" << k << " T=" << time;
      previous = current;
    }
  }
}

// The 1/sqrt(T) decay: a three-week skew is steep, a two-year skew nearly flat.
TEST(Chain, SkewFlattensAsMaturityExtends) {
  const SkewParameters skew{};
  const double k = 0.05;

  double previous_slope = std::fabs(
      (skew_volatility(skew, k, 0.08) - skew_volatility(skew, -k, 0.08)) / (2.0 * k));
  for (const double time : {0.25, 1.0, 2.0}) {
    const double slope = std::fabs(
        (skew_volatility(skew, k, time) - skew_volatility(skew, -k, time)) / (2.0 * k));
    EXPECT_LT(slope, previous_slope) << "T=" << time;
    previous_slope = slope;
  }
}

// The wing term is quadratic near the money and linear far out. The crossover
// is what keeps generated chains free of static arbitrage; a plain quadratic
// grows fast enough to make call prices rise with strike at long tenors.
TEST(Chain, WingTermIsQuadraticNearTheMoneyAndLinearFarOut) {
  SkewParameters skew{};
  skew.slope_one_year = 0.0;
  skew.atm_vol_short = skew.atm_vol_long_run;

  const double base = atm_volatility(skew, 1.0);
  const auto wing = [&](double k) { return skew_volatility(skew, k, 1.0) - base; };

  // Near the money: matches curvature / (2 * wing_width) * k^2.
  const double quadratic_coefficient = skew.curvature / (2.0 * skew.wing_width);
  for (const double k : {0.01, 0.02, 0.04}) {
    EXPECT_NEAR(wing(k), quadratic_coefficient * k * k, 0.02 * quadratic_coefficient * k * k)
        << "k=" << k;
  }

  // Far out: the increment per unit of |k| approaches the asymptotic slope.
  const double far_increment = (wing(6.0) - wing(5.0)) / 1.0;
  EXPECT_NEAR(far_increment, skew.curvature, 1e-3);

  EXPECT_NEAR(wing(0.30), wing(-0.30), 1e-15) << "wing term must be symmetric in k";
}

TEST(Chain, AtmVolatilityMeanRevertsTowardTheLongRunLevel) {
  const SkewParameters skew{};
  EXPECT_NEAR(atm_volatility(skew, 0.0), skew.atm_vol_short, 1e-12);
  EXPECT_NEAR(atm_volatility(skew, 50.0), skew.atm_vol_long_run, 1e-6);
  EXPECT_LT(atm_volatility(skew, 0.25), atm_volatility(skew, 2.0));
}

TEST(Chain, LongerTenorsSpanAWiderStrikeBand) {
  const OptionChain chain = generate_chain(ChainConfig{});

  double previous_width = 0.0;
  for (const ExpirySlice& slice : chain.expiries) {
    const std::vector<ContractQuote> calls = QuotesOfType(slice, OptionType::Call);
    ASSERT_GE(calls.size(), 2u);
    const double width =
        std::log(calls.back().strike / calls.front().strike);
    EXPECT_GT(width, previous_width) << "T=" << slice.time;
    previous_width = width;
  }
}

TEST(Chain, StrikesLieOnTheConfiguredIncrement) {
  ChainConfig config{};
  config.strike_increment = 25.0;
  const OptionChain chain = generate_chain(config);

  for (const ExpirySlice& slice : chain.expiries) {
    for (const ContractQuote& quote : slice.quotes) {
      EXPECT_NEAR(std::fmod(quote.strike, config.strike_increment), 0.0, 1e-9)
          << "strike=" << quote.strike;
    }
  }
}

TEST(Chain, MarketsBracketTheoAndBidsNeverGoNegative) {
  const OptionChain chain = generate_chain(ChainConfig{});
  for (const ExpirySlice& slice : chain.expiries) {
    for (const ContractQuote& quote : slice.quotes) {
      EXPECT_GE(quote.bid, 0.0);
      EXPECT_LT(quote.bid, quote.ask);
      EXPECT_LE(quote.mid, quote.ask);
    }
  }
}

// Quoting in volatility terms means the price-space half-spread tracks vega,
// which is what makes wing markets narrow in price and near-the-money markets
// wide -- the opposite of a fixed price spread.
TEST(Chain, HalfSpreadsTrackVegaRatherThanPrice) {
  ChainConfig config{};
  config.minimum_half_spread = 0.0;
  const OptionChain chain = generate_chain(config);

  const ExpirySlice& slice = chain.expiries[3];
  for (const ContractQuote& quote : slice.quotes) {
    const ForwardInputs in{.forward = slice.forward,
                           .strike = quote.strike,
                           .discount_factor = slice.discount_factor,
                           .volatility = quote.generating_volatility,
                           .time = slice.time};
    const double expected = forward_vega(in) * config.half_spread_vol_points;
    EXPECT_NEAR(quote.ask - quote.mid, expected, 1e-9) << "K=" << quote.strike;
  }
}

// Both sides of each strike are priced from the same volatility, so the chain
// satisfies parity exactly and the M2 regression has an exact answer to find.
TEST(Chain, NoiselessChainsAreExactlyParityConsistent) {
  const OptionChain chain = generate_chain(ChainConfig{});

  for (const ExpirySlice& slice : chain.expiries) {
    const ParityFit fit = fit_forward_and_discount(ToParityQuotes(slice));
    ASSERT_TRUE(fit.ok()) << "T=" << slice.time;
    EXPECT_NEAR(fit.forward, slice.forward, 1e-6) << "T=" << slice.time;
    EXPECT_NEAR(fit.discount_factor, slice.discount_factor, 1e-9) << "T=" << slice.time;
    EXPECT_NEAR(fit.r_squared, 1.0, 1e-10) << "T=" << slice.time;
  }
}

// The full M2 + M3 pipeline. Generate a chain from a known surface, recover
// the forward and discount from the chain alone, then invert the
// out-of-the-money side back into volatilities and compare against the
// surface that produced them.
TEST(Chain, InvertingAChainRecoversTheGeneratingSurface) {
  const OptionChain chain = generate_chain(ChainConfig{});

  for (const ExpirySlice& slice : chain.expiries) {
    const ParityFit fit = fit_forward_and_discount(ToParityQuotes(slice));
    ASSERT_TRUE(fit.ok());

    int checked = 0;
    for (const ContractQuote& quote : slice.quotes) {
      if (quote.type != out_of_the_money_type(fit.forward, quote.strike)) {
        continue;
      }

      const ImpliedVolQuery query{.forward = fit.forward,
                                  .strike = quote.strike,
                                  .discount_factor = fit.discount_factor,
                                  .time = slice.time};
      const ImpliedVolResult recovered =
          implied_volatility(quote.mid, query, quote.type);

      ASSERT_TRUE(recovered.ok())
          << "T=" << slice.time << " K=" << quote.strike
          << " status=" << static_cast<int>(recovered.status);
      EXPECT_NEAR(recovered.volatility, quote.generating_volatility, 1e-6)
          << "T=" << slice.time << " K=" << quote.strike;
      ++checked;
    }
    EXPECT_GT(checked, 0) << "no out-of-the-money quotes at T=" << slice.time;
  }
}

// Call prices must be convex in strike, or a butterfly spread would have a
// negative cost and pay a non-negative payoff. M4 fits a surface to these
// chains, so the input has to be free of static arbitrage before the fit's own
// arbitrage checks mean anything.
TEST(Chain, CallPricesAreConvexAndDecreasingInStrike) {
  const OptionChain chain = generate_chain(ChainConfig{});

  for (const ExpirySlice& slice : chain.expiries) {
    const std::vector<ContractQuote> calls = QuotesOfType(slice, OptionType::Call);
    ASSERT_GE(calls.size(), 3u);

    for (std::size_t i = 1; i < calls.size(); ++i) {
      EXPECT_LT(calls[i].mid, calls[i - 1].mid)
          << "T=" << slice.time << " K=" << calls[i].strike;
    }

    // General convexity for a non-uniform strike ladder: the middle price must
    // sit at or below the chord joining its neighbours.
    for (std::size_t i = 1; i + 1 < calls.size(); ++i) {
      const double k_low = calls[i - 1].strike;
      const double k_mid = calls[i].strike;
      const double k_high = calls[i + 1].strike;
      const double chord = ((k_high - k_mid) * calls[i - 1].mid +
                            (k_mid - k_low) * calls[i + 1].mid) /
                           (k_high - k_low);
      EXPECT_LE(calls[i].mid, chord + 1e-9)
          << "butterfly arbitrage at T=" << slice.time << " K=" << k_mid;
    }
  }
}

// Independent perturbation of each contract breaks parity slightly, which is
// exactly the condition the regression's R-squared exists to detect.
TEST(Chain, QuoteNoiseDegradesParityFitQualityWithoutBiasingTheForward) {
  ChainConfig config{};
  config.quote_noise_vol_points = 0.006;
  const OptionChain chain = generate_chain(config);

  const ExpirySlice& slice = chain.expiries[3];
  const ParityFit fit = fit_forward_and_discount(ToParityQuotes(slice));

  ASSERT_TRUE(fit.ok());
  EXPECT_LT(fit.r_squared, 1.0 - 1e-12) << "noise left the fit perfect";
  EXPECT_GT(fit.r_squared, 0.99);
  EXPECT_NEAR(fit.forward, slice.forward, 5.0);
}

}  // namespace
}  // namespace skewdesk
