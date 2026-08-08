#pragma once

#include <cstddef>
#include <span>

namespace skewdesk {

// Gatheral's raw SVI parameterization of total implied variance against
// log-moneyness k = ln(strike / forward):
//
//   w(k) = a + b * (rho * (k - m) + sqrt((k - m)^2 + sigma^2))
//
// Total variance rather than volatility, because total variance is the
// quantity that arbitrage conditions are naturally expressed in: no-calendar
// arbitrage is simply that w is non-decreasing in maturity at fixed k, and
// Durrleman's butterfly condition is a statement about w and its first two
// derivatives.
//
// Geometrically the function is a hyperbola: two straight asymptotes whose
// slopes are b*(rho - 1) on the left and b*(rho + 1) on the right, joined
// smoothly around a vertex near k = m. So each parameter maps onto something
// a trader already thinks about -- a is the overall level, b the angle between
// the wings, rho the asymmetry that produces skew, m the horizontal position
// of the minimum, and sigma how rounded the vertex is.
struct SviParameters {
  double a{};
  double b{};
  double rho{};
  double m{};
  double sigma{};
};

[[nodiscard]] double total_variance(const SviParameters& p, double k) noexcept;
[[nodiscard]] double total_variance_slope(const SviParameters& p, double k) noexcept;
[[nodiscard]] double total_variance_curvature(const SviParameters& p, double k) noexcept;

// Black-Scholes implied volatility implied by the slice at this log-moneyness.
// Named distinctly from the M2 solver, which answers the opposite question:
// that one inverts a quoted price, this one evaluates a fitted surface.
[[nodiscard]] double svi_volatility(const SviParameters& p, double k, double time) noexcept;

// The smallest total variance the slice attains anywhere, which is where a
// slice goes negative if it is going to.
[[nodiscard]] double minimum_total_variance(const SviParameters& p) noexcept;

// Durrleman's function. A slice is free of butterfly arbitrage exactly when
// this is non-negative everywhere:
//
//   g(k) = (1 - k w'/(2w))^2 - (w'^2/4)(1/w + 1/4) + w''/2
//
// What it really is: g(k) is proportional to the risk-neutral probability
// density implied by the slice. A negative value is a negative probability,
// which shows up in tradeable terms as a butterfly spread that costs less than
// nothing. Checking the density directly is stronger than spot-checking
// butterflies at the quoted strikes, because it catches violations between
// strikes that no listed butterfly would reveal.
[[nodiscard]] double durrleman_function(const SviParameters& p, double k) noexcept;

struct ButterflyCheck {
  bool free_of_arbitrage{};
  double worst_value{};
  double worst_log_moneyness{};
};

[[nodiscard]] ButterflyCheck check_butterfly(const SviParameters& p, double k_low,
                                             double k_high, int samples = 401) noexcept;

// One quoted point, already converted into the surface's own coordinates.
struct SviObservation {
  double log_moneyness{};
  double total_variance{};
  double weight{1.0};
};

enum class SviFitStatus {
  Success,
  // Five free parameters need at least five points to be determined.
  InsufficientObservations,
  // Non-finite or non-positive total variances in the input.
  DegenerateObservations,
  // A slice was fitted, but it implies negative probability density
  // somewhere. Parameters are still returned so the caller can inspect and
  // report, but the surface must not be quoted from.
  ButterflyArbitrage,
};

struct SviFitSettings {
  // The outer search runs over (m, sigma) only; for any fixed pair the
  // remaining three parameters fall out of a linear least squares, so the
  // whole problem is a two-dimensional search over a cheap objective. That is
  // why a deterministic shrinking grid is used rather than a general-purpose
  // optimizer -- there is no pathological geometry to defend against, and a
  // grid gives byte-identical results across compilers and platforms.
  int grid_points{21};
  int refinements{5};
  double sigma_low{1e-3};
  double sigma_high{1.0};
  // How far beyond the observed strike range the vertex is allowed to sit.
  double m_padding{0.5};
  // Extra log-moneyness beyond the observed range to check for butterfly
  // arbitrage, since a fitted slice is often extrapolated a little.
  double arbitrage_check_padding{0.25};
  int arbitrage_check_samples{401};
};

struct SviFit {
  SviParameters parameters{};
  double time{};
  double rmse{};
  ButterflyCheck butterfly{};
  SviFitStatus status{SviFitStatus::InsufficientObservations};

  [[nodiscard]] bool ok() const noexcept { return status == SviFitStatus::Success; }
};

[[nodiscard]] SviFit fit_svi(std::span<const SviObservation> observations, double time,
                             const SviFitSettings& settings = {});

}  // namespace skewdesk
