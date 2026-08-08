#pragma once

#include "skewdesk/chain.hpp"
#include "skewdesk/svi.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace skewdesk {

// No-calendar-spread arbitrage is the requirement that total variance never
// decreases with maturity at a fixed log-moneyness. If a nearer expiry carried
// more total variance than a further one at the same strike, a calendar spread
// would have negative cost and a non-negative payoff.
//
// Note this is checked in total variance and at fixed log-moneyness, not in
// implied volatility and not at fixed strike. Implied volatility routinely
// falls with maturity without any arbitrage at all -- that is just the term
// structure -- so a check phrased in volatility would raise false alarms on
// perfectly ordinary surfaces.
struct CalendarCheck {
  bool free_of_arbitrage{};
  double worst_gap{};
  double worst_log_moneyness{};
  std::size_t earlier_slice{};
  std::size_t later_slice{};
};

struct SurfaceSlice {
  double time{};
  double forward{};
  double discount_factor{};
  double parity_r_squared{};
  // Observed log-moneyness range, used to decide where the slice is trusted.
  double k_low{};
  double k_high{};
  int observations_used{};
  int quotes_rejected{};
  SviFit fit{};
};

enum class SurfaceStatus {
  Success,
  NoUsableExpiries,
  SliceFitFailed,
  ButterflyArbitrage,
  CalendarArbitrage,
};

struct SurfaceFitSettings {
  SviFitSettings svi{};
  // Weighting observations by vega concentrates the fit where quotes are
  // informative. A far-wing option's price barely moves with volatility, so
  // its implied volatility is enormously sensitive to a tick of quote noise;
  // weighting by vega stops those points from dragging the whole slice.
  bool weight_by_vega{true};
  int calendar_check_samples{201};
};

struct VolSurface {
  std::vector<SurfaceSlice> slices{};
  CalendarCheck calendar{};
  SurfaceStatus status{SurfaceStatus::NoUsableExpiries};

  [[nodiscard]] bool ok() const noexcept { return status == SurfaceStatus::Success; }

  // Black-Scholes volatility at an arbitrary maturity and log-moneyness.
  //
  // Interpolation is linear in *total variance* against maturity, which is the
  // only choice that cannot manufacture calendar arbitrage between two
  // arbitrage-free slices. Interpolating volatility instead would.
  //
  // Outside the quoted maturities, total variance is scaled proportionally to
  // maturity from the nearest slice, which holds implied volatility flat --
  // the conventional extrapolation, and the one that stays monotone in total
  // variance.
  [[nodiscard]] double volatility_at(double time, double log_moneyness) const noexcept;
  [[nodiscard]] double total_variance_at(double time, double log_moneyness) const noexcept;
};

[[nodiscard]] CalendarCheck check_calendar(std::span<const SurfaceSlice> slices,
                                           int samples = 201) noexcept;

// The full pipeline: for each expiry, recover the forward and discount from
// put-call parity, invert the out-of-the-money quotes into implied
// volatilities, fit an SVI slice to the resulting total variances, then check
// the assembled surface for butterfly and calendar arbitrage.
[[nodiscard]] VolSurface fit_surface(const OptionChain& chain,
                                     const SurfaceFitSettings& settings = {});

}  // namespace skewdesk
