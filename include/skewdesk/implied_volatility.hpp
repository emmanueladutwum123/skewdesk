#pragma once

#include "skewdesk/black_scholes.hpp"

namespace skewdesk {

// Everything the market gives you about a contract except the one number you
// are solving for.
struct ImpliedVolQuery {
  double forward{};
  double strike{};
  double discount_factor{};
  double time{};
};

enum class ImpliedVolStatus {
  Success,
  // The quote is cheaper than the option's own discounted intrinsic value.
  // No volatility reproduces it, because volatility cannot be negative --
  // this is a static-arbitrage violation in the input, not a solver failure,
  // and it is common in real chains where a stale or crossed quote slips
  // through.
  PriceBelowIntrinsic,
  // Above the no-arbitrage ceiling: a call cannot exceed the discounted
  // forward, nor a put the discounted strike.
  PriceAboveUpperBound,
  // Non-positive time, forward, strike, or discount factor -- the contract
  // itself is not well-formed.
  DegenerateContract,
  // The quote does not pin down a volatility, so no answer would be
  // meaningful. This is the deep in-the-money case: essentially all of the
  // option's value is intrinsic, and the time value that carries the
  // volatility information has fallen below the resolution of a double at
  // that magnitude. At forward 4500, strike 3000, and three weeks to expiry,
  // volatilities of 8%, 15% and 30% all produce byte-identical prices -- the
  // inversion is not merely inaccurate, it is impossible.
  //
  // The remedy is the one desks already use: invert the *out-of-the-money*
  // option at that strike, whose value is entirely time value and therefore
  // well conditioned, and obtain the other side from put-call parity. See
  // out_of_the_money_type().
  IllConditioned,
  // The quote sits inside the bounds but the bracket did not close within the
  // iteration budget.
  DidNotConverge,
};

struct ImpliedVolResult {
  double volatility{};
  ImpliedVolStatus status{ImpliedVolStatus::DidNotConverge};
  int iterations{};

  [[nodiscard]] bool ok() const noexcept { return status == ImpliedVolStatus::Success; }
};

struct ImpliedVolSettings {
  // 500%. Index options do not trade anywhere near this even in a crisis; the
  // ceiling exists to bound the bisection bracket, not to express a view.
  double max_volatility{5.0};
  // Convergence is judged on the volatility step rather than on the pricing
  // error. A price-based criterion is scale-dependent and silently wrong in
  // the far wings, where an option worth 1e-20 would satisfy any reasonable
  // absolute price tolerance at sigma = 0 and the solver would return zero
  // for a contract whose true implied volatility is perfectly ordinary.
  double volatility_tolerance{1e-12};
  // Slack on the no-arbitrage bound checks, relative to discount * forward.
  double bound_tolerance{1e-12};
  int max_iterations{100};
};

[[nodiscard]] ForwardInputs with_volatility(const ImpliedVolQuery& query,
                                            double volatility) noexcept;

// Which side of a strike is out of the money against a given forward, and so
// which of the pair should be inverted. Calls above the forward and puts below
// it hold their entire value as time value, which is what makes them the
// well-conditioned half; the in-the-money side is dominated by intrinsic value
// that tells you nothing about volatility. Surface construction should quote
// off this side and recover the other through parity.
[[nodiscard]] OptionType out_of_the_money_type(double forward, double strike) noexcept;

// Recovers the Black-Scholes volatility that reproduces `market_price`.
//
// Uses Newton-Raphson safeguarded by bisection: a Newton step is taken only
// when it lands inside the current bracket and is shrinking the interval fast
// enough, and the method falls back to bisection otherwise. That safeguard is
// what makes the far wings tractable -- vega collapses toward zero there, so
// an unguarded Newton step divides by something near zero and diverges. The
// safeguard conditions are written so that a zero vega naturally forces
// bisection rather than requiring a special case.
[[nodiscard]] ImpliedVolResult implied_volatility(double market_price,
                                                  const ImpliedVolQuery& query,
                                                  OptionType type,
                                                  const ImpliedVolSettings& settings = {}) noexcept;

}  // namespace skewdesk
