#include "skewdesk/quoting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skewdesk {
namespace {

// The slice whose expiry is closest to the contract's. Used only to read the
// fit residual for the uncertainty term; pricing itself interpolates across
// slices properly.
[[nodiscard]] const SurfaceSlice* nearest_slice(const VolSurface& surface,
                                                double time) noexcept {
  const SurfaceSlice* best = nullptr;
  double best_distance = std::numeric_limits<double>::infinity();
  for (const SurfaceSlice& slice : surface.slices) {
    const double distance = std::fabs(slice.time - time);
    if (distance < best_distance) {
      best_distance = distance;
      best = &slice;
    }
  }
  return best;
}

// The slice fit's RMSE is in total variance; quoted width is in volatility.
// Since w = sigma^2 * T, a perturbation converts as dsigma = dw / (2 sigma T).
[[nodiscard]] double uncertainty_in_vol_terms(double rmse, double volatility,
                                              double time) noexcept {
  const double denominator = 2.0 * volatility * time;
  return (denominator > 0.0) ? rmse / denominator : 0.0;
}

}  // namespace

Quote make_quote(const ContractId& contract, const VolSurface& surface,
                 const PortfolioRisk& risk, const QuoteSettings& settings) {
  Quote quote{};
  quote.contract = contract;

  const std::size_t tenor_count = settings.risk.buckets.tenor_bucket_count();
  const std::size_t moneyness_count = settings.risk.buckets.moneyness_bucket_count();
  if (risk.vega_buckets.grid.size() != tenor_count ||
      risk.vega_buckets.grid.empty() ||
      risk.vega_buckets.grid.front().size() != moneyness_count) {
    quote.status = QuoteStatus::RiskBucketMismatch;
    return quote;
  }

  if (surface.slices.empty() || surface.spot <= 0.0 || contract.time <= 0.0 ||
      contract.strike <= 0.0) {
    quote.status = QuoteStatus::NotPriceable;
    return quote;
  }

  const double forward = surface.forward_at(contract.time);
  const double discount = surface.discount_factor_at(contract.time);
  if (forward <= 0.0 || discount <= 0.0) {
    quote.status = QuoteStatus::NotPriceable;
    return quote;
  }

  const double log_moneyness = std::log(contract.strike / forward);
  const double theo_vol = surface.volatility_at(contract.time, log_moneyness);
  if (!(theo_vol > 0.0)) {
    quote.status = QuoteStatus::NotPriceable;
    return quote;
  }

  const std::size_t tenor = bucket_index(contract.time, settings.risk.buckets.tenor_edges);
  const std::size_t moneyness =
      bucket_index(log_moneyness, settings.risk.buckets.log_moneyness_edges);
  const double bucket_vega = risk.vega_buckets.grid[tenor][moneyness];

  const double raw_utilisation =
      (settings.bucket_vega_limit > 0.0) ? bucket_vega / settings.bucket_vega_limit : 0.0;
  const double utilisation = std::clamp(raw_utilisation, -1.0, 1.0);
  quote.inventory_utilisation = utilisation;

  // Long the bucket quotes it lower, so the flow it attracts sells to the
  // maker rather than buying from it.
  const double skew = -settings.max_skew_vol_points * utilisation;

  const SurfaceSlice* slice = nearest_slice(surface, contract.time);
  const double uncertainty =
      (slice != nullptr)
          ? settings.uncertainty_width_multiplier *
                uncertainty_in_vol_terms(slice->fit.rmse, theo_vol, contract.time)
          : 0.0;

  const double half_width_vol = settings.base_half_width_vol_points +
                                settings.inventory_width_vol_points * std::fabs(utilisation) +
                                uncertainty;

  quote.theoretical_volatility = theo_vol;
  quote.quoted_mid_volatility = std::fmax(theo_vol + skew, 1e-6);
  quote.bid_volatility = std::fmax(quote.quoted_mid_volatility - half_width_vol, 0.0);
  quote.ask_volatility = quote.quoted_mid_volatility + half_width_vol;

  const auto price_at = [&](double volatility) {
    const ForwardInputs in{.forward = forward,
                           .strike = contract.strike,
                           .discount_factor = discount,
                           .volatility = volatility,
                           .time = contract.time};
    return price(in, contract.type);
  };

  quote.theoretical_price = price_at(theo_vol);
  const double quoted_mid_price = price_at(quote.quoted_mid_volatility);
  quote.bid = price_at(quote.bid_volatility);
  quote.ask = price_at(quote.ask_volatility);

  // A width set purely in volatility collapses to nothing in price terms where
  // vega is negligible -- short-dated and deep-wing contracts. Those are
  // precisely the ones a maker cannot afford to show a one-tick market in.
  if (quote.ask - quote.bid < 2.0 * settings.minimum_half_width) {
    quote.bid = quoted_mid_price - settings.minimum_half_width;
    quote.ask = quoted_mid_price + settings.minimum_half_width;
  }
  quote.bid = std::fmax(quote.bid, 0.0);
  if (quote.ask <= quote.bid) {
    quote.ask = quote.bid + 2.0 * settings.minimum_half_width;
  }

  // Sizes taper on the side that would add to the position and stay full on
  // the side that would reduce it; at the limit the adding side is withdrawn
  // altogether.
  const double bid_scale = std::clamp(1.0 - std::fmax(raw_utilisation, 0.0), 0.0, 1.0);
  const double ask_scale = std::clamp(1.0 + std::fmin(raw_utilisation, 0.0), 0.0, 1.0);

  quote.bid_size = bid_scale > 0.0
                       ? std::fmax(settings.base_size * bid_scale, settings.minimum_size)
                       : 0.0;
  quote.ask_size = ask_scale > 0.0
                       ? std::fmax(settings.base_size * ask_scale, settings.minimum_size)
                       : 0.0;

  quote.status = QuoteStatus::Success;
  return quote;
}

std::vector<Quote> quote_ladder(std::span<const ContractId> contracts,
                                const VolSurface& surface, const PortfolioRisk& risk,
                                const QuoteSettings& settings) {
  std::vector<Quote> quotes;
  quotes.reserve(contracts.size());
  for (const ContractId& contract : contracts) {
    quotes.push_back(make_quote(contract, surface, risk, settings));
  }
  return quotes;
}

}  // namespace skewdesk
