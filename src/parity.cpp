#include "skewdesk/parity.hpp"

#include <cmath>
#include <cstddef>

namespace skewdesk {

ParityFit fit_forward_and_discount(std::span<const ParityQuote> quotes) noexcept {
  ParityFit fit{};

  if (quotes.size() < 2) {
    fit.status = ParityStatus::InsufficientQuotes;
    return fit;
  }

  const auto n = static_cast<double>(quotes.size());

  double sum_x = 0.0;
  double sum_y = 0.0;
  for (const ParityQuote& q : quotes) {
    sum_x += q.strike;
    sum_y += q.call_price - q.put_price;
  }
  const double mean_x = sum_x / n;
  const double mean_y = sum_y / n;

  // Centred accumulation rather than the raw sum-of-squares form: strikes on
  // an index chain are large numbers in a narrow band (4000-5000, say), so
  // sum_xx and n*mean_x^2 agree to many digits and subtracting them throws
  // away most of the precision in the variance.
  double cov = 0.0;
  double var_x = 0.0;
  for (const ParityQuote& q : quotes) {
    const double dx = q.strike - mean_x;
    cov += dx * ((q.call_price - q.put_price) - mean_y);
    var_x += dx * dx;
  }

  if (var_x <= 0.0) {
    fit.status = ParityStatus::DegenerateStrikes;
    return fit;
  }

  const double slope = cov / var_x;
  const double intercept = mean_y - slope * mean_x;

  if (slope >= 0.0) {
    fit.status = ParityStatus::NonPositiveDiscountFactor;
    return fit;
  }

  fit.discount_factor = -slope;
  fit.forward = intercept / fit.discount_factor;

  double residual_ss = 0.0;
  double total_ss = 0.0;
  for (const ParityQuote& q : quotes) {
    const double y = q.call_price - q.put_price;
    const double predicted = intercept + slope * q.strike;
    residual_ss += (y - predicted) * (y - predicted);
    total_ss += (y - mean_y) * (y - mean_y);
  }
  // A flat response has no variance to explain; call the fit perfect when the
  // residuals vanish too, and worthless otherwise, rather than dividing by
  // zero.
  fit.r_squared = (total_ss > 0.0) ? 1.0 - residual_ss / total_ss
                                   : (residual_ss <= 0.0 ? 1.0 : 0.0);

  fit.status = ParityStatus::Success;
  return fit;
}

}  // namespace skewdesk
