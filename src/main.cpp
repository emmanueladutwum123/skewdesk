#include "skewdesk/chain.hpp"
#include "skewdesk/portfolio.hpp"
#include "skewdesk/surface.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

// Runs the whole pipeline end to end: generate a chain from a known
// volatility surface, recover each expiry's forward and discount factor from
// put-call parity alone, invert the out-of-the-money quotes, fit an SVI slice
// to each expiry, and report what the arbitrage checks say about the result.
int main() {
  const skewdesk::ChainConfig config{};
  const skewdesk::OptionChain chain = skewdesk::generate_chain(config);
  const skewdesk::VolSurface surface = skewdesk::fit_surface(chain);

  std::printf("Fitted surface  --  %zu expiries, status=%s\n\n", surface.slices.size(),
              surface.ok() ? "arbitrage-free" : "REJECTED");

  std::printf("%6s %9s %5s %8s %8s %7s %8s %8s %11s %10s\n", "T", "forward", "pts", "a", "b",
              "rho", "m", "sigma", "rmse(w)", "min g");
  std::printf(
      "%s\n",
      "-------------------------------------------------------------------------------------------");

  for (const skewdesk::SurfaceSlice& slice : surface.slices) {
    const skewdesk::SviParameters& p = slice.fit.parameters;
    std::printf("%6.2f %9.2f %5d %8.5f %8.5f %7.3f %8.4f %8.4f %11.2e %10.4f\n", slice.time,
                slice.forward, slice.observations_used, p.a, p.b, p.rho, p.m, p.sigma,
                slice.fit.rmse, slice.fit.butterfly.worst_value);
  }

  std::printf("\nCalendar check: worst total-variance gap = %+.6e at k = %+.3f\n",
              surface.calendar.worst_gap, surface.calendar.worst_log_moneyness);

  // How closely the fitted surface reproduces the surface the chain was built
  // from -- the number that actually matters, since everything above could be
  // internally consistent and still wrong.
  double worst_vol_error = 0.0;
  double worst_time = 0.0;
  double worst_k = 0.0;
  for (const skewdesk::SurfaceSlice& slice : surface.slices) {
    for (int i = 0; i <= 20; ++i) {
      const double position = static_cast<double>(i) / 20.0;
      const double k = slice.k_low + position * (slice.k_high - slice.k_low);
      const double error =
          std::fabs(skewdesk::svi_volatility(slice.fit.parameters, k, slice.time) -
                    skewdesk::skew_volatility(config.skew, k, slice.time));
      if (error > worst_vol_error) {
        worst_vol_error = error;
        worst_time = slice.time;
        worst_k = k;
      }
    }
  }
  std::printf("Worst reproduction error vs the generating surface: %.6f absolute"
              " volatility (%.2f vol points) at T=%.2f, k=%+.3f\n",
              worst_vol_error, worst_vol_error * 100.0, worst_time, worst_k);

  // A book whose net vega says almost nothing about the risk it carries: long
  // front-month volatility against short two-year, plus a downside skew
  // position. Both are invisible in the aggregate and obvious in the grid.
  skewdesk::PositionBook book;
  book.add({.time = 0.08, .strike = 4400.0, .type = skewdesk::OptionType::Put}, 400.0);
  book.add({.time = 0.08, .strike = 4600.0, .type = skewdesk::OptionType::Call}, 250.0);
  book.add({.time = 0.25, .strike = 4100.0, .type = skewdesk::OptionType::Put}, -180.0);
  book.add({.time = 1.00, .strike = 4500.0, .type = skewdesk::OptionType::Call}, -60.0);
  book.add({.time = 2.00, .strike = 5200.0, .type = skewdesk::OptionType::Call}, -45.0);

  const skewdesk::RiskSettings settings{};
  const skewdesk::PortfolioRisk risk = skewdesk::compute_risk(book, surface, settings);

  std::printf("\n\nBook risk  --  %d positions priced, %d skipped\n", risk.positions_priced,
              risk.positions_skipped);
  std::printf("  value %+.0f   delta %+.1f   gamma %+.4f   theta/day %+.0f\n", risk.value,
              risk.delta, risk.gamma, risk.theta / 365.0);
  std::printf("  net vega %+.0f   term-weighted vega %+.0f   GROSS vega %.0f\n", risk.vega,
              risk.weighted_vega, risk.gross_vega);

  // Buckets are half-open [lower, upper), so a contract sitting exactly on an
  // edge belongs to the bucket above it.
  std::printf("\nVega by tenor x log-moneyness (net vega is %.1f%% of gross)\n",
              100.0 * std::fabs(risk.vega) / risk.gross_vega);
  std::printf("%14s %12s %12s %12s %12s %12s\n", "", "k < -0.10", "[-.10, -.03)",
              "[-.03, .03)", "[.03, .10)", "k >= 0.10");

  const std::vector<const char*> tenor_labels = {"     [0, 1M)", "    [1M, 3M)",
                                                 "    [3M, 6M)", "    [6M, 1Y)",
                                                 "    [1Y, 2Y)", "   [2Y, inf)"};
  for (std::size_t t = 0; t < risk.vega_buckets.grid.size(); ++t) {
    std::printf("%14s", tenor_labels[t]);
    for (const double cell : risk.vega_buckets.grid[t]) {
      std::printf(" %12.0f", cell);
    }
    std::printf("   | %+.0f\n", risk.vega_buckets.by_tenor[t]);
  }

  return 0;
}
