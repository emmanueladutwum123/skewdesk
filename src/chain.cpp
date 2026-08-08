#include "skewdesk/chain.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <random>
#include <utility>

namespace skewdesk {
namespace {

[[nodiscard]] double round_to_increment(double value, double increment) noexcept {
  if (increment <= 0.0) {
    return value;
  }
  return std::round(value / increment) * increment;
}

}  // namespace

double atm_volatility(const SkewParameters& skew, double time) noexcept {
  return skew.atm_vol_long_run +
         (skew.atm_vol_short - skew.atm_vol_long_run) *
             std::exp(-skew.atm_reversion * time);
}

double skew_volatility(const SkewParameters& skew, double log_moneyness,
                       double time) noexcept {
  // The 1/sqrt(T) decay of the slope is the empirical regularity that makes a
  // three-week skew steep and a two-year skew nearly flat.
  const double slope =
      (time > 0.0) ? skew.slope_one_year / std::sqrt(time) : skew.slope_one_year;
  // Smoothed absolute value: quadratic near the money, linear in the wings.
  // See the header for why a plain quadratic produces call prices that rise
  // with strike at long tenors.
  const double wing =
      skew.curvature * (std::hypot(log_moneyness, skew.wing_width) - skew.wing_width);
  const double vol = atm_volatility(skew, time) + slope * log_moneyness + wing;
  return std::fmax(vol, skew.minimum_vol);
}

OptionChain generate_chain(const ChainConfig& config) {
  OptionChain chain{};
  chain.spot = config.spot;

  std::mt19937_64 rng{config.seed};
  std::normal_distribution<double> noise{0.0, config.quote_noise_vol_points};

  for (const double time : config.tenors) {
    ExpirySlice slice{};
    slice.time = time;
    slice.forward = config.spot * std::exp((config.rate - config.dividend) * time);
    slice.discount_factor = std::exp(-config.rate * time);

    // Strikes are laid out in standard deviations of log-moneyness, so the
    // band widens with maturity the way a real chain's does, then snapped to
    // the exchange's strike increment. Snapping can collide neighbouring
    // samples at short tenors, where the whole band is only a few increments
    // wide; keeping the ladder strictly increasing drops those duplicates.
    const double std_dev = atm_volatility(config.skew, time) * std::sqrt(time);
    const double k_bound = config.strike_span_in_sd * std_dev;
    const int samples = std::max(config.strikes_per_expiry, 2);

    std::vector<double> strikes;
    strikes.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
      const double position =
          static_cast<double>(i) / static_cast<double>(samples - 1);
      const double log_moneyness = -k_bound + position * 2.0 * k_bound;
      const double strike = round_to_increment(slice.forward * std::exp(log_moneyness),
                                               config.strike_increment);
      if (strike > 0.0 && (strikes.empty() || strike > strikes.back())) {
        strikes.push_back(strike);
      }
    }

    for (const double strike : strikes) {
      const double model_vol =
          skew_volatility(config.skew, std::log(strike / slice.forward), time);

      for (const OptionType type : {OptionType::Call, OptionType::Put}) {
        double volatility = model_vol;
        if (config.quote_noise_vol_points > 0.0) {
          volatility = std::fmax(volatility + noise(rng), config.skew.minimum_vol);
        }

        const ForwardInputs in{.forward = slice.forward,
                               .strike = strike,
                               .discount_factor = slice.discount_factor,
                               .volatility = volatility,
                               .time = time};

        const double theo = price(in, type);
        const double half_spread =
            std::fmax(forward_vega(in) * config.half_spread_vol_points,
                      config.minimum_half_spread);

        ContractQuote quote{};
        quote.strike = strike;
        quote.type = type;
        // `mid` is the theoretical value, not the midpoint of the quoted
        // market. The two diverge in the deep wings once the bid clamps at
        // zero -- a real artifact of real chains, and the reason wing
        // mid-market quotes are a poor volatility input.
        quote.mid = theo;
        quote.bid = std::fmax(theo - half_spread, 0.0);
        quote.ask = theo + half_spread;
        quote.generating_volatility = volatility;

        slice.quotes.push_back(quote);
      }
    }

    chain.expiries.push_back(std::move(slice));
  }

  return chain;
}

}  // namespace skewdesk
