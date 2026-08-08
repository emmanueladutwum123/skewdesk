#pragma once

namespace skewdesk {

enum class OptionType { Call, Put };

// Inputs to the Black-Scholes-Merton model for a European option on an asset
// paying a continuous dividend yield -- the standard setup for equity index
// options, where the index's dividend stream is close enough to continuous
// that modelling discrete payments buys very little.
//
// Rates and yields are continuously compounded; `time` is a year fraction.
struct BlackScholesInputs {
  double spot{};
  double strike{};
  double rate{};        // continuously-compounded risk-free rate
  double dividend{};    // continuous dividend yield
  double volatility{};  // annualized
  double time{};        // year fraction to expiry
};

// Sensitivities in their conventional trading units:
//   delta  per 1.00 move in spot
//   gamma  delta change per 1.00 move in spot
//   vega   per 1.00 of volatility (i.e. 100 vol points, not one)
//   theta  per year (negative for a long option -- value decays)
//   rho    per 1.00 of interest rate
//
// Desks usually rescale vega and theta at the display layer (per vol point,
// per day). That scaling is deliberately left out here so the numbers compose
// cleanly with finite differences and with each other; presentation is a
// separate concern from the model.
struct Greeks {
  double delta{};
  double gamma{};
  double vega{};
  double theta{};
  double rho{};
};

[[nodiscard]] double norm_pdf(double x) noexcept;
[[nodiscard]] double norm_cdf(double x) noexcept;

// The forward price of the underlying at expiry, and the discount factor
// applied to the payoff.
//
// Pricing is expressed on a forward basis internally rather than in raw spot
// terms. The two are algebraically identical, but the forward form is the one
// that survives contact with real chain data: for index options the effective
// forward and discount are *recovered from the market* via put-call parity
// rather than assumed from a quoted rate and dividend yield. Structuring the
// model this way now means M2 can substitute a parity-implied forward without
// touching the pricing core.
[[nodiscard]] double forward_price(const BlackScholesInputs& in) noexcept;
[[nodiscard]] double discount_factor(const BlackScholesInputs& in) noexcept;

// Undefined inputs (non-positive time, volatility, spot, or strike) are not
// errors -- they are limit cases with well-defined values. The payoff becomes
// deterministic and the option is worth its discounted intrinsic value on the
// forward. See `greeks` for what that implies for the sensitivities.
[[nodiscard]] double price(const BlackScholesInputs& in, OptionType type) noexcept;

// Analytic sensitivities. In the deterministic limit described above, delta is
// a step function (reported as the dividend-discounted indicator of being
// in-the-money) and the remaining greeks are reported as zero: gamma and vega
// genuinely vanish, while theta and rho sit on a kink where no finite value is
// correct. Returning zero there is a deliberate convention, not an oversight.
[[nodiscard]] Greeks greeks(const BlackScholesInputs& in, OptionType type) noexcept;

}  // namespace skewdesk
