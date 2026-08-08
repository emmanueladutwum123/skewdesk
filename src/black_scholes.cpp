#include "skewdesk/black_scholes.hpp"

#include <cmath>

namespace skewdesk {
namespace {

constexpr double kInvSqrt2 = 0.70710678118654752440;
constexpr double kInvSqrt2Pi = 0.39894228040143267794;

// True when the payoff carries no uncertainty, so d1/d2 -- which divide by
// sigma*sqrt(T) -- are undefined rather than merely large. Non-positive
// forward or strike are folded in here too: both make log(F/K) undefined, and
// both have unambiguous limiting values that the deterministic branch computes
// correctly (a zero-forward call is worthless; a zero-strike call is the
// discounted forward).
[[nodiscard]] bool is_deterministic(const ForwardInputs& in) noexcept {
  return in.time <= 0.0 || in.volatility <= 0.0 || in.forward <= 0.0 || in.strike <= 0.0;
}

struct Moments {
  double d1;
  double d2;
};

[[nodiscard]] Moments moments(const ForwardInputs& in) noexcept {
  const double std_dev = in.volatility * std::sqrt(in.time);
  const double d1 = (std::log(in.forward / in.strike) + 0.5 * std_dev * std_dev) / std_dev;
  return Moments{d1, d1 - std_dev};
}

}  // namespace

double norm_pdf(double x) noexcept { return kInvSqrt2Pi * std::exp(-0.5 * x * x); }

double norm_cdf(double x) noexcept {
  // erfc rather than the more familiar 0.5 * (1 + erf(x / sqrt(2))): for x
  // well below zero that form computes 1 + (something within an ulp of -1)
  // and catastrophically cancels, losing most of its significant digits.
  // erfc(-x / sqrt(2)) stays accurate far into the left tail -- which is
  // exactly where deep out-of-the-money options live, and those are a large
  // share of any index option chain.
  return 0.5 * std::erfc(-x * kInvSqrt2);
}

double forward_price(const BlackScholesInputs& in) noexcept {
  return in.spot * std::exp((in.rate - in.dividend) * in.time);
}

double discount_factor(const BlackScholesInputs& in) noexcept {
  return std::exp(-in.rate * in.time);
}

ForwardInputs to_forward(const BlackScholesInputs& in) noexcept {
  return ForwardInputs{.forward = forward_price(in),
                       .strike = in.strike,
                       .discount_factor = discount_factor(in),
                       .volatility = in.volatility,
                       .time = in.time};
}

double price(const ForwardInputs& in, OptionType type) noexcept {
  if (is_deterministic(in)) {
    const double intrinsic =
        (type == OptionType::Call) ? in.forward - in.strike : in.strike - in.forward;
    return in.discount_factor * std::fmax(intrinsic, 0.0);
  }

  const auto [d1, d2] = moments(in);

  if (type == OptionType::Call) {
    return in.discount_factor * (in.forward * norm_cdf(d1) - in.strike * norm_cdf(d2));
  }
  return in.discount_factor * (in.strike * norm_cdf(-d2) - in.forward * norm_cdf(-d1));
}

double forward_vega(const ForwardInputs& in) noexcept {
  if (is_deterministic(in)) {
    return 0.0;
  }
  return in.discount_factor * in.forward * norm_pdf(moments(in).d1) * std::sqrt(in.time);
}

// The spot parameterization is a thin adapter over the forward one: the two
// are algebraically identical, and keeping a single implementation means the
// parity-recovered forward used by later milestones exercises exactly the same
// code path as the textbook spot form the tests pin down.
double price(const BlackScholesInputs& in, OptionType type) noexcept {
  return price(to_forward(in), type);
}

Greeks greeks(const BlackScholesInputs& in, OptionType type) noexcept {
  Greeks g{};

  const ForwardInputs fwd_in = to_forward(in);
  const double df = fwd_in.discount_factor;
  const double div_df = std::exp(-in.dividend * in.time);

  if (is_deterministic(fwd_in) || in.spot <= 0.0) {
    const bool itm =
        (type == OptionType::Call) ? fwd_in.forward > in.strike : fwd_in.forward < in.strike;
    if (itm) {
      g.delta = (type == OptionType::Call) ? div_df : -div_df;
    }
    return g;
  }

  const auto [d1, d2] = moments(fwd_in);
  const double sqrt_t = std::sqrt(in.time);
  const double pdf_d1 = norm_pdf(d1);

  // Gamma and vega are identical for a call and a put at the same strike:
  // put-call parity differs by (discounted forward - discounted strike),
  // which is linear in spot and free of volatility, so it contributes nothing
  // to a second spot derivative or to any volatility derivative.
  g.gamma = div_df * pdf_d1 / (in.spot * in.volatility * sqrt_t);
  g.vega = in.spot * div_df * pdf_d1 * sqrt_t;

  // The pure time-decay term, shared by calls and puts. The remaining theta
  // components are the carry on the two legs of the parity relation, which is
  // why they flip sign between the two option types.
  const double decay = -(in.spot * div_df * pdf_d1 * in.volatility) / (2.0 * sqrt_t);

  if (type == OptionType::Call) {
    g.delta = div_df * norm_cdf(d1);
    g.theta = decay - in.rate * in.strike * df * norm_cdf(d2) +
              in.dividend * in.spot * div_df * norm_cdf(d1);
    g.rho = in.strike * in.time * df * norm_cdf(d2);
  } else {
    g.delta = -div_df * norm_cdf(-d1);
    g.theta = decay + in.rate * in.strike * df * norm_cdf(-d2) -
              in.dividend * in.spot * div_df * norm_cdf(-d1);
    g.rho = -in.strike * in.time * df * norm_cdf(-d2);
  }

  return g;
}

}  // namespace skewdesk
