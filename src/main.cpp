#include "skewdesk/chain.hpp"
#include "skewdesk/surface.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

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

  return 0;
}
