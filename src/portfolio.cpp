#include "skewdesk/portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace skewdesk {
namespace {

constexpr double kQuantityEpsilon = 1e-12;

[[nodiscard]] bool same_contract(const ContractId& lhs, const ContractId& rhs) noexcept {
  return lhs.type == rhs.type && lhs.strike == rhs.strike && lhs.time == rhs.time;
}

}  // namespace

void PositionBook::add(const ContractId& contract, double quantity) {
  const auto found = std::find_if(
      positions_.begin(), positions_.end(),
      [&](const Position& position) { return same_contract(position.contract, contract); });

  if (found == positions_.end()) {
    if (std::fabs(quantity) > kQuantityEpsilon) {
      positions_.push_back(Position{.contract = contract, .quantity = quantity});
    }
    return;
  }

  found->quantity += quantity;
  if (std::fabs(found->quantity) <= kQuantityEpsilon) {
    positions_.erase(found);
  }
}

void PositionBook::clear() noexcept { positions_.clear(); }

double PositionBook::quantity_of(const ContractId& contract) const noexcept {
  const auto found = std::find_if(
      positions_.begin(), positions_.end(),
      [&](const Position& position) { return same_contract(position.contract, contract); });
  return (found == positions_.end()) ? 0.0 : found->quantity;
}

std::size_t bucket_index(double value, std::span<const double> edges) noexcept {
  const auto upper = std::upper_bound(edges.begin(), edges.end(), value);
  return static_cast<std::size_t>(upper - edges.begin());
}

PortfolioRisk compute_risk(const PositionBook& book, const VolSurface& surface,
                           const RiskSettings& settings) {
  PortfolioRisk risk{};

  const std::size_t tenor_count = settings.buckets.tenor_bucket_count();
  const std::size_t moneyness_count = settings.buckets.moneyness_bucket_count();
  risk.vega_buckets.by_tenor.assign(tenor_count, 0.0);
  risk.vega_buckets.by_log_moneyness.assign(moneyness_count, 0.0);
  risk.vega_buckets.grid.assign(tenor_count, std::vector<double>(moneyness_count, 0.0));

  if (surface.slices.empty() || surface.spot <= 0.0) {
    risk.positions_skipped = static_cast<int>(book.size());
    return risk;
  }

  for (const Position& position : book.positions()) {
    const ContractId& contract = position.contract;
    const double forward = surface.forward_at(contract.time);
    const double discount = surface.discount_factor_at(contract.time);

    if (contract.time <= 0.0 || contract.strike <= 0.0 || forward <= 0.0 ||
        discount <= 0.0) {
      ++risk.positions_skipped;
      continue;
    }

    const double log_moneyness = std::log(contract.strike / forward);
    const double volatility = surface.volatility_at(contract.time, log_moneyness);
    if (!(volatility > 0.0)) {
      ++risk.positions_skipped;
      continue;
    }

    const ForwardInputs forward_inputs{.forward = forward,
                                       .strike = contract.strike,
                                       .discount_factor = discount,
                                       .volatility = volatility,
                                       .time = contract.time};

    // Reuse the spot-parameterization greeks rather than restating the
    // formulas in forward terms: those are the ones checked against finite
    // differences, and the conversion back is exact.
    const BlackScholesInputs spot_inputs = from_forward(surface.spot, forward_inputs);
    if (spot_inputs.time <= 0.0) {
      ++risk.positions_skipped;
      continue;
    }

    const Greeks greeks_of = greeks(spot_inputs, contract.type);
    const double scale = position.quantity * settings.multiplier;

    risk.value += scale * price(forward_inputs, contract.type);
    risk.delta += scale * greeks_of.delta;
    risk.gamma += scale * greeks_of.gamma;
    risk.theta += scale * greeks_of.theta;

    const double position_vega = scale * greeks_of.vega;
    risk.vega += position_vega;
    risk.weighted_vega +=
        position_vega * std::sqrt(settings.weighted_vega_reference_time / contract.time);

    const std::size_t tenor = bucket_index(contract.time, settings.buckets.tenor_edges);
    const std::size_t moneyness =
        bucket_index(log_moneyness, settings.buckets.log_moneyness_edges);
    risk.vega_buckets.by_tenor[tenor] += position_vega;
    risk.vega_buckets.by_log_moneyness[moneyness] += position_vega;
    risk.vega_buckets.grid[tenor][moneyness] += position_vega;

    ++risk.positions_priced;
  }

  for (const std::vector<double>& row : risk.vega_buckets.grid) {
    for (const double cell : row) {
      risk.gross_vega += std::fabs(cell);
    }
  }

  return risk;
}

}  // namespace skewdesk
