#include "skewdesk/surface.hpp"

#include "skewdesk/implied_volatility.hpp"
#include "skewdesk/parity.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace skewdesk {
namespace {

std::vector<ParityQuote> pair_by_strike(const ExpirySlice& slice) {
  std::map<double, ParityQuote> paired;
  for (const ContractQuote& quote : slice.quotes) {
    ParityQuote& entry = paired[quote.strike];
    entry.strike = quote.strike;
    if (quote.type == OptionType::Call) {
      entry.call_price = quote.mid;
    } else {
      entry.put_price = quote.mid;
    }
  }

  std::vector<ParityQuote> result;
  result.reserve(paired.size());
  for (const auto& entry : paired) {
    result.push_back(entry.second);
  }
  return result;
}

}  // namespace

double VolSurface::total_variance_at(double time, double log_moneyness) const noexcept {
  if (slices.empty() || time <= 0.0) {
    return 0.0;
  }

  const SurfaceSlice& first = slices.front();
  if (time <= first.time) {
    return total_variance(first.fit.parameters, log_moneyness) * (time / first.time);
  }

  const SurfaceSlice& last = slices.back();
  if (time >= last.time) {
    return total_variance(last.fit.parameters, log_moneyness) * (time / last.time);
  }

  for (std::size_t i = 1; i < slices.size(); ++i) {
    const SurfaceSlice& earlier = slices[i - 1];
    const SurfaceSlice& later = slices[i];
    if (time > later.time) {
      continue;
    }
    const double span = later.time - earlier.time;
    const double position = (span > 0.0) ? (time - earlier.time) / span : 0.0;
    const double w_earlier = total_variance(earlier.fit.parameters, log_moneyness);
    const double w_later = total_variance(later.fit.parameters, log_moneyness);
    return w_earlier + position * (w_later - w_earlier);
  }

  return total_variance(last.fit.parameters, log_moneyness);
}

double VolSurface::volatility_at(double time, double log_moneyness) const noexcept {
  if (time <= 0.0) {
    return 0.0;
  }
  return std::sqrt(std::fmax(total_variance_at(time, log_moneyness), 0.0) / time);
}

CalendarCheck check_calendar(std::span<const SurfaceSlice> slices, int samples) noexcept {
  CalendarCheck check{};
  check.worst_gap = std::numeric_limits<double>::infinity();
  check.free_of_arbitrage = true;

  if (slices.size() < 2) {
    check.worst_gap = 0.0;
    return check;
  }

  const int count = std::max(samples, 2);
  for (std::size_t i = 1; i < slices.size(); ++i) {
    const SurfaceSlice& earlier = slices[i - 1];
    const SurfaceSlice& later = slices[i];

    // Only compare where both slices were actually quoted; beyond that the
    // curves are extrapolations and a crossing there says nothing about the
    // market.
    const double k_low = std::fmax(earlier.k_low, later.k_low);
    const double k_high = std::fmin(earlier.k_high, later.k_high);
    if (!(k_high > k_low)) {
      continue;
    }

    for (int s = 0; s < count; ++s) {
      const double position = static_cast<double>(s) / static_cast<double>(count - 1);
      const double k = k_low + position * (k_high - k_low);
      const double gap = total_variance(later.fit.parameters, k) -
                         total_variance(earlier.fit.parameters, k);
      if (gap < check.worst_gap) {
        check.worst_gap = gap;
        check.worst_log_moneyness = k;
        check.earlier_slice = i - 1;
        check.later_slice = i;
      }
    }
  }

  if (!std::isfinite(check.worst_gap)) {
    check.worst_gap = 0.0;
  }
  check.free_of_arbitrage = check.worst_gap >= 0.0;
  return check;
}

VolSurface fit_surface(const OptionChain& chain, const SurfaceFitSettings& settings) {
  VolSurface surface{};

  for (const ExpirySlice& expiry : chain.expiries) {
    const ParityFit parity = fit_forward_and_discount(pair_by_strike(expiry));
    if (!parity.ok()) {
      continue;
    }

    SurfaceSlice slice{};
    slice.time = expiry.time;
    slice.forward = parity.forward;
    slice.discount_factor = parity.discount_factor;
    slice.parity_r_squared = parity.r_squared;
    slice.k_low = std::numeric_limits<double>::infinity();
    slice.k_high = -std::numeric_limits<double>::infinity();

    std::vector<SviObservation> observations;
    observations.reserve(expiry.quotes.size() / 2);

    for (const ContractQuote& quote : expiry.quotes) {
      // Only the out-of-the-money side is invertible with any precision; the
      // in-the-money side is dominated by intrinsic value that carries no
      // information about volatility. See ImpliedVolStatus::IllConditioned.
      if (quote.type != out_of_the_money_type(parity.forward, quote.strike)) {
        continue;
      }

      const ImpliedVolQuery query{.forward = parity.forward,
                                  .strike = quote.strike,
                                  .discount_factor = parity.discount_factor,
                                  .time = expiry.time};
      const ImpliedVolResult inverted = implied_volatility(quote.mid, query, quote.type);
      if (!inverted.ok()) {
        ++slice.quotes_rejected;
        continue;
      }

      const double k = std::log(quote.strike / parity.forward);
      SviObservation observation{};
      observation.log_moneyness = k;
      observation.total_variance = inverted.volatility * inverted.volatility * expiry.time;
      observation.weight =
          settings.weight_by_vega
              ? std::fmax(forward_vega(with_volatility(query, inverted.volatility)), 1e-12)
              : 1.0;

      observations.push_back(observation);
      slice.k_low = std::fmin(slice.k_low, k);
      slice.k_high = std::fmax(slice.k_high, k);
    }

    slice.observations_used = static_cast<int>(observations.size());
    slice.fit = fit_svi(observations, expiry.time, settings.svi);
    surface.slices.push_back(std::move(slice));
  }

  if (surface.slices.empty()) {
    surface.status = SurfaceStatus::NoUsableExpiries;
    return surface;
  }

  std::sort(surface.slices.begin(), surface.slices.end(),
            [](const SurfaceSlice& lhs, const SurfaceSlice& rhs) {
              return lhs.time < rhs.time;
            });

  surface.calendar = check_calendar(surface.slices, settings.calendar_check_samples);

  bool any_fit_failure = false;
  bool any_butterfly = false;
  for (const SurfaceSlice& slice : surface.slices) {
    if (slice.fit.status == SviFitStatus::ButterflyArbitrage) {
      any_butterfly = true;
    } else if (!slice.fit.ok()) {
      any_fit_failure = true;
    }
  }

  if (any_fit_failure) {
    surface.status = SurfaceStatus::SliceFitFailed;
  } else if (any_butterfly) {
    surface.status = SurfaceStatus::ButterflyArbitrage;
  } else if (!surface.calendar.free_of_arbitrage) {
    surface.status = SurfaceStatus::CalendarArbitrage;
  } else {
    surface.status = SurfaceStatus::Success;
  }

  return surface;
}

}  // namespace skewdesk
