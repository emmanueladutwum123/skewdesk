#pragma once

#include <span>

namespace skewdesk {

// A call and a put quoted at the same strike and the same expiry.
struct ParityQuote {
  double strike{};
  double call_price{};
  double put_price{};
};

enum class ParityStatus {
  Success,
  // Fewer than two quotes: a line through one point is not determined.
  InsufficientQuotes,
  // Every quote sits at the same strike, so the regressor has no variance.
  DegenerateStrikes,
  // The fitted slope was non-negative, implying a discount factor at or below
  // zero. Parity guarantees a negative slope, so this means the input is not
  // a coherent same-expiry chain -- mixed expiries, or calls and puts that
  // are not actually paired.
  NonPositiveDiscountFactor,
};

struct ParityFit {
  double forward{};
  double discount_factor{};
  // Coefficient of determination of the regression. Put-call parity is an
  // exact arbitrage relation, so clean same-expiry data fits essentially
  // perfectly; anything materially below 1 means the quotes disagree with
  // each other and the recovered forward should not be trusted.
  double r_squared{};
  ParityStatus status{ParityStatus::InsufficientQuotes};

  [[nodiscard]] bool ok() const noexcept { return status == ParityStatus::Success; }
};

// Recovers the forward and the discount factor for one expiry from its own
// option chain, with no reference to a quoted interest rate or dividend yield.
//
// Put-call parity says C - K = discount * (forward - strike), so regressing
// (call - put) on strike across the chain gives a line whose slope is
// -discount and whose intercept is discount * forward. This is what desks
// actually do: for an equity index neither the financing rate nor the dividend
// stream is directly observable, but the chain prices them jointly, and every
// strike is an independent observation of the same two unknowns. Fitting
// across all of them rather than inverting a single pair also averages away
// quote noise.
[[nodiscard]] ParityFit fit_forward_and_discount(std::span<const ParityQuote> quotes) noexcept;

}  // namespace skewdesk
