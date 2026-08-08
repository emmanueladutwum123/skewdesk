#include "skewdesk/implied_volatility.hpp"

#include <cmath>
#include <limits>
#include <numbers>

namespace skewdesk {
namespace {

[[nodiscard]] bool is_degenerate(const ImpliedVolQuery& q) noexcept {
  return q.time <= 0.0 || q.forward <= 0.0 || q.strike <= 0.0 || q.discount_factor <= 0.0;
}

[[nodiscard]] double intrinsic(const ImpliedVolQuery& q, OptionType type) noexcept {
  const double payoff =
      (type == OptionType::Call) ? q.forward - q.strike : q.strike - q.forward;
  return q.discount_factor * std::fmax(payoff, 0.0);
}

// A call is worth at most the discounted forward (it can never beat owning the
// asset outright); a put at most the discounted strike (its payoff is capped
// when the underlying goes to zero).
[[nodiscard]] double upper_bound(const ImpliedVolQuery& q, OptionType type) noexcept {
  return q.discount_factor * ((type == OptionType::Call) ? q.forward : q.strike);
}

// Brenner-Subrahmanyam: at the money, the Black-Scholes price is very nearly
// linear in volatility with slope discount * forward / sqrt(2*pi/T), so this
// inverts to a good starting point. Away from the money it degrades, which is
// exactly why the bisection safeguard exists -- the guess only has to be
// somewhere sane, not close.
[[nodiscard]] double initial_guess(double market_price, const ImpliedVolQuery& q) noexcept {
  const double undiscounted = market_price / q.discount_factor;
  return (undiscounted / q.forward) * std::sqrt(2.0 * std::numbers::pi / q.time);
}

}  // namespace

OptionType out_of_the_money_type(double forward, double strike) noexcept {
  return (strike >= forward) ? OptionType::Call : OptionType::Put;
}

ForwardInputs with_volatility(const ImpliedVolQuery& query, double volatility) noexcept {
  return ForwardInputs{.forward = query.forward,
                       .strike = query.strike,
                       .discount_factor = query.discount_factor,
                       .volatility = volatility,
                       .time = query.time};
}

ImpliedVolResult implied_volatility(double market_price, const ImpliedVolQuery& query,
                                    OptionType type,
                                    const ImpliedVolSettings& settings) noexcept {
  ImpliedVolResult result{};

  if (is_degenerate(query) || !std::isfinite(market_price)) {
    result.status = ImpliedVolStatus::DegenerateContract;
    return result;
  }

  const double scale = query.discount_factor * query.forward;
  const double slack = settings.bound_tolerance * scale;

  const double floor_price = intrinsic(query, type);
  if (market_price < floor_price - slack) {
    result.status = ImpliedVolStatus::PriceBelowIntrinsic;
    return result;
  }
  if (market_price > upper_bound(query, type) + slack) {
    result.status = ImpliedVolStatus::PriceAboveUpperBound;
    return result;
  }

  // Everything the volatility can influence lives in the time value; the
  // intrinsic part is fixed. So the question of whether this quote is
  // invertible at all is whether its time value survives being stored
  // alongside its intrinsic value in the same double.
  //
  // Note that the test is relative to the intrinsic value, not an absolute
  // price tolerance. An absolute tolerance is meaningless here: a far-wing
  // option legitimately worth 1e-73 would fall inside any absolute slack wide
  // enough to be useful near the money, and the solver would report zero
  // volatility for a contract whose implied volatility is perfectly ordinary.
  // Out of the money the intrinsic value is zero, so the threshold collapses
  // to "is the quote strictly positive" -- which is exactly right.
  const double time_value = market_price - floor_price;
  const double resolution = 8.0 * std::numeric_limits<double>::epsilon() * floor_price;
  if (time_value <= resolution) {
    result.status = ImpliedVolStatus::IllConditioned;
    return result;
  }

  // Price is strictly increasing in volatility, so f(sigma) = price(sigma) -
  // target is negative at the low end of the bracket and positive at the high
  // end. That monotonicity is what guarantees the bracket always contains the
  // root and that bisection can never lose it.
  double low = 0.0;
  double high = settings.max_volatility;

  if (price(with_volatility(query, high), type) < market_price) {
    result.status = ImpliedVolStatus::DidNotConverge;
    return result;
  }

  double sigma = initial_guess(market_price, query);
  if (!(sigma > low && sigma < high)) {
    sigma = 0.5 * (low + high);
  }

  double step_before_last = high - low;
  double step = step_before_last;
  double f = price(with_volatility(query, sigma), type) - market_price;
  double vega = forward_vega(with_volatility(query, sigma));

  for (int i = 1; i <= settings.max_iterations; ++i) {
    result.iterations = i;

    // Two rejection conditions, both from the classic safeguarded-Newton
    // formulation. The first fires when the Newton step would land outside
    // the bracket; the second when it is not at least halving the interval.
    // With vega at or near zero -- the deep-wing case -- the first product is
    // f*f > 0 and the second reduces to |2f| > 0, so both fire and the method
    // degrades to pure bisection without any explicit guard.
    const bool out_of_bracket =
        ((sigma - high) * vega - f) * ((sigma - low) * vega - f) > 0.0;
    const bool too_slow = std::fabs(2.0 * f) > std::fabs(step_before_last * vega);

    step_before_last = step;
    if (out_of_bracket || too_slow) {
      step = 0.5 * (high - low);
      sigma = low + step;
    } else {
      step = f / vega;
      sigma -= step;
    }

    if (std::fabs(step) < settings.volatility_tolerance) {
      result.volatility = sigma;
      result.status = ImpliedVolStatus::Success;
      return result;
    }

    f = price(with_volatility(query, sigma), type) - market_price;
    vega = forward_vega(with_volatility(query, sigma));

    if (f < 0.0) {
      low = sigma;
    } else {
      high = sigma;
    }
  }

  result.volatility = sigma;
  result.status = ImpliedVolStatus::DidNotConverge;
  return result;
}

}  // namespace skewdesk
