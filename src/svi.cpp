#include "skewdesk/svi.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace skewdesk {
namespace {

constexpr double kRhoBound = 1.0 - 1e-9;
constexpr double kVarianceFloor = 1e-12;

// Solves a 3x3 symmetric system by Gaussian elimination with partial
// pivoting. Small and dense enough that a general solver would be more code
// than it saves.
[[nodiscard]] bool solve3(double matrix[3][3], double rhs[3], double out[3]) noexcept {
  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 3; ++row) {
      if (std::fabs(matrix[row][col]) > std::fabs(matrix[pivot][col])) {
        pivot = row;
      }
    }
    if (std::fabs(matrix[pivot][col]) < 1e-14) {
      return false;
    }
    if (pivot != col) {
      for (int k = 0; k < 3; ++k) {
        std::swap(matrix[col][k], matrix[pivot][k]);
      }
      std::swap(rhs[col], rhs[pivot]);
    }
    for (int row = col + 1; row < 3; ++row) {
      const double factor = matrix[row][col] / matrix[col][col];
      for (int k = col; k < 3; ++k) {
        matrix[row][k] -= factor * matrix[col][k];
      }
      rhs[row] -= factor * rhs[col];
    }
  }
  for (int row = 2; row >= 0; --row) {
    double sum = rhs[row];
    for (int k = row + 1; k < 3; ++k) {
      sum -= matrix[row][k] * out[k];
    }
    out[row] = sum / matrix[row][row];
  }
  return true;
}

// Maps the reduced coefficients back to raw SVI and pulls them into the region
// where the slice is a valid variance curve: non-negative wing angle,
// correlation strictly inside (-1, 1), and a level high enough that total
// variance never goes negative. Butterfly arbitrage is deliberately *not*
// enforced here -- it is checked and reported afterwards, so a violation
// surfaces as a fact about the market data rather than being silently
// massaged away by the fitter.
[[nodiscard]] SviParameters to_raw(double a, double d, double c, double m,
                                   double sigma) noexcept {
  SviParameters p{};
  p.m = m;
  p.sigma = sigma;
  p.b = std::fmax(c / sigma, 0.0);
  p.rho = (c > 0.0) ? std::clamp(d / c, -kRhoBound, kRhoBound) : 0.0;

  const double floor_a = -p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
  p.a = std::fmax(a, floor_a + kVarianceFloor);
  return p;
}

struct Candidate {
  SviParameters parameters{};
  double objective{std::numeric_limits<double>::infinity()};
};

// For a fixed vertex position and width, SVI is linear in its three remaining
// parameters. That collapse is the whole reason this fit is tractable: the
// outer search only has two dimensions, and every point in it is an exact
// least-squares solution rather than another optimizer's guess.
[[nodiscard]] Candidate evaluate(std::span<const SviObservation> observations, double m,
                                 double sigma) noexcept {
  double matrix[3][3] = {};
  double rhs[3] = {};

  for (const SviObservation& obs : observations) {
    const double y = (obs.log_moneyness - m) / sigma;
    const double z = std::sqrt(y * y + 1.0);
    const double basis[3] = {1.0, y, z};

    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        matrix[i][j] += obs.weight * basis[i] * basis[j];
      }
      rhs[i] += obs.weight * basis[i] * obs.total_variance;
    }
  }

  double solution[3] = {};
  if (!solve3(matrix, rhs, solution)) {
    return Candidate{};
  }

  // With y = (k - m) / sigma the model reads
  //   w = a + (b*rho*sigma) * y + (b*sigma) * sqrt(y^2 + 1)
  // so the regression coefficients are exactly (a, b*rho*sigma, b*sigma) and
  // to_raw divides the third by sigma to recover b. Scaling them by sigma
  // before that division leaves b too large by a factor of sigma -- which the
  // outer search then hides by distorting sigma, producing a fit that looks
  // plausible and is measurably wrong.
  Candidate candidate{};
  candidate.parameters = to_raw(solution[0], solution[1], solution[2], m, sigma);

  // Scored against the clamped parameters, not the raw least-squares solution.
  // Clamping changes the curve, so an objective computed before it would rank
  // candidates by a fit that is not the one being returned.
  double objective = 0.0;
  for (const SviObservation& obs : observations) {
    const double residual =
        total_variance(candidate.parameters, obs.log_moneyness) - obs.total_variance;
    objective += obs.weight * residual * residual;
  }
  candidate.objective = objective;
  return candidate;
}

}  // namespace

double total_variance(const SviParameters& p, double k) noexcept {
  const double y = k - p.m;
  return p.a + p.b * (p.rho * y + std::hypot(y, p.sigma));
}

double total_variance_slope(const SviParameters& p, double k) noexcept {
  const double y = k - p.m;
  const double root = std::hypot(y, p.sigma);
  return (root > 0.0) ? p.b * (p.rho + y / root) : p.b * p.rho;
}

double total_variance_curvature(const SviParameters& p, double k) noexcept {
  const double y = k - p.m;
  const double root = std::hypot(y, p.sigma);
  return (root > 0.0) ? p.b * p.sigma * p.sigma / (root * root * root) : 0.0;
}

double svi_volatility(const SviParameters& p, double k, double time) noexcept {
  if (time <= 0.0) {
    return 0.0;
  }
  return std::sqrt(std::fmax(total_variance(p, k), 0.0) / time);
}

double minimum_total_variance(const SviParameters& p) noexcept {
  return p.a + p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
}

double durrleman_function(const SviParameters& p, double k) noexcept {
  const double w = total_variance(p, k);
  if (w <= 0.0) {
    return -std::numeric_limits<double>::infinity();
  }
  const double dw = total_variance_slope(p, k);
  const double ddw = total_variance_curvature(p, k);

  const double first = 1.0 - k * dw / (2.0 * w);
  return first * first - 0.25 * dw * dw * (1.0 / w + 0.25) + 0.5 * ddw;
}

ButterflyCheck check_butterfly(const SviParameters& p, double k_low, double k_high,
                               int samples) noexcept {
  ButterflyCheck check{};
  check.worst_value = std::numeric_limits<double>::infinity();
  check.worst_log_moneyness = k_low;

  const int count = std::max(samples, 2);
  for (int i = 0; i < count; ++i) {
    const double position = static_cast<double>(i) / static_cast<double>(count - 1);
    const double k = k_low + position * (k_high - k_low);
    const double g = durrleman_function(p, k);
    if (g < check.worst_value) {
      check.worst_value = g;
      check.worst_log_moneyness = k;
    }
  }

  check.free_of_arbitrage = check.worst_value >= 0.0;
  return check;
}

SviFit fit_svi(std::span<const SviObservation> observations, double time,
               const SviFitSettings& settings) {
  SviFit fit{};
  fit.time = time;

  if (observations.size() < 5) {
    fit.status = SviFitStatus::InsufficientObservations;
    return fit;
  }

  double k_min = std::numeric_limits<double>::infinity();
  double k_max = -std::numeric_limits<double>::infinity();
  for (const SviObservation& obs : observations) {
    if (!std::isfinite(obs.log_moneyness) || !std::isfinite(obs.total_variance) ||
        obs.total_variance <= 0.0 || obs.weight < 0.0) {
      fit.status = SviFitStatus::DegenerateObservations;
      return fit;
    }
    k_min = std::fmin(k_min, obs.log_moneyness);
    k_max = std::fmax(k_max, obs.log_moneyness);
  }
  if (k_max - k_min <= 0.0) {
    fit.status = SviFitStatus::DegenerateObservations;
    return fit;
  }

  double m_low = k_min - settings.m_padding;
  double m_high = k_max + settings.m_padding;
  // sigma spans orders of magnitude, so the search walks it geometrically.
  double log_sigma_low = std::log(settings.sigma_low);
  double log_sigma_high = std::log(settings.sigma_high);

  const int points = std::max(settings.grid_points, 3);
  Candidate best{};

  for (int pass = 0; pass <= std::max(settings.refinements, 0); ++pass) {
    const double m_step = (m_high - m_low) / static_cast<double>(points - 1);
    const double log_sigma_step =
        (log_sigma_high - log_sigma_low) / static_cast<double>(points - 1);

    int best_i = 0;
    int best_j = 0;
    Candidate pass_best{};

    for (int i = 0; i < points; ++i) {
      const double m = m_low + static_cast<double>(i) * m_step;
      for (int j = 0; j < points; ++j) {
        const double sigma = std::exp(log_sigma_low + static_cast<double>(j) * log_sigma_step);
        const Candidate candidate = evaluate(observations, m, sigma);
        if (candidate.objective < pass_best.objective) {
          pass_best = candidate;
          best_i = i;
          best_j = j;
        }
      }
    }

    if (pass_best.objective < best.objective) {
      best = pass_best;
    }

    // Shrink to one grid step either side of the winner and search again.
    const double m_centre = m_low + static_cast<double>(best_i) * m_step;
    const double log_sigma_centre =
        log_sigma_low + static_cast<double>(best_j) * log_sigma_step;
    m_low = m_centre - m_step;
    m_high = m_centre + m_step;
    log_sigma_low = std::fmax(log_sigma_centre - log_sigma_step, std::log(1e-6));
    log_sigma_high = log_sigma_centre + log_sigma_step;
  }

  if (!std::isfinite(best.objective)) {
    fit.status = SviFitStatus::DegenerateObservations;
    return fit;
  }

  fit.parameters = best.parameters;

  // Reported unweighted, so it reads as a plain typical error in total
  // variance rather than in whatever units the weighting scheme happens to
  // impose.
  double squared_error = 0.0;
  for (const SviObservation& obs : observations) {
    const double residual =
        total_variance(fit.parameters, obs.log_moneyness) - obs.total_variance;
    squared_error += residual * residual;
  }
  fit.rmse = std::sqrt(squared_error / static_cast<double>(observations.size()));

  fit.butterfly = check_butterfly(fit.parameters, k_min - settings.arbitrage_check_padding,
                                  k_max + settings.arbitrage_check_padding,
                                  settings.arbitrage_check_samples);

  fit.status = fit.butterfly.free_of_arbitrage ? SviFitStatus::Success
                                               : SviFitStatus::ButterflyArbitrage;
  return fit;
}

}  // namespace skewdesk
