#include "skewdesk/simulation.hpp"

#include "skewdesk/implied_volatility.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace skewdesk {
namespace {

// Below this much time remaining a contract is treated as expired: greeks
// become unbounded and the volatility surface stops meaning anything.
constexpr double kMinimumRemainingTime = 1e-4;

struct SimPosition {
  double expiry{};
  double strike{};
  OptionType type{};
  double quantity{};
};

struct PendingMarkout {
  int due_step{};
  double expiry{};
  double strike{};
  OptionType type{};
  double size{};
  // +1 when the maker sold, -1 when it bought.
  double maker_sign{};
  double trade_price{};
  double theo_at_trade{};
  bool informed{};
};

[[nodiscard]] double market_volatility(const SimulationConfig& config, double log_moneyness,
                                       double remaining, double level) noexcept {
  return std::fmax(skew_volatility(config.skew, log_moneyness, remaining) + level,
                   config.minimum_volatility);
}

// Builds the maker's view of the world at a point in the run: one SVI slice
// per live expiry, fitted to the true surface sampled across that expiry's
// strike ladder.
[[nodiscard]] VolSurface build_surface(const SimulationConfig& config, double now,
                                       double spot, double level) {
  VolSurface surface{};
  surface.spot = spot;

  for (const double expiry : config.expiries) {
    const double remaining = expiry - now;
    if (remaining <= kMinimumRemainingTime) {
      continue;
    }

    SurfaceSlice slice{};
    slice.time = remaining;
    slice.forward = spot * std::exp((config.rate - config.dividend) * remaining);
    slice.discount_factor = std::exp(-config.rate * remaining);
    slice.parity_r_squared = 1.0;

    const double std_dev =
        market_volatility(config, 0.0, remaining, level) * std::sqrt(remaining);
    const double bound = config.strike_span_in_sd * std_dev;
    const int samples = std::max(config.strikes_per_expiry, 5);

    std::vector<SviObservation> observations;
    observations.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
      const double position =
          static_cast<double>(i) / static_cast<double>(samples - 1);
      const double k = -bound + position * 2.0 * bound;
      const double volatility = market_volatility(config, k, remaining, level);
      observations.push_back(SviObservation{
          .log_moneyness = k, .total_variance = volatility * volatility * remaining,
          .weight = 1.0});
    }

    slice.k_low = -bound;
    slice.k_high = bound;
    slice.observations_used = samples;
    slice.fit = fit_svi(observations, remaining, config.fit);
    surface.slices.push_back(std::move(slice));
  }

  if (surface.slices.empty()) {
    surface.status = SurfaceStatus::NoUsableExpiries;
    return surface;
  }

  surface.calendar = check_calendar(surface.slices);
  surface.status = SurfaceStatus::Success;
  return surface;
}

[[nodiscard]] PositionBook build_book(const std::vector<SimPosition>& positions,
                                      double now) {
  PositionBook book;
  for (const SimPosition& position : positions) {
    const double remaining = position.expiry - now;
    if (remaining <= kMinimumRemainingTime) {
      continue;
    }
    book.add(ContractId{.time = remaining,
                        .strike = position.strike,
                        .type = position.type},
             position.quantity);
  }
  return book;
}

// Strike ladder for one expiry, on the exchange's increment.
[[nodiscard]] std::vector<double> strike_ladder(const SimulationConfig& config,
                                                double forward, double remaining,
                                                double level) {
  const double std_dev =
      market_volatility(config, 0.0, remaining, level) * std::sqrt(remaining);
  const double bound = config.strike_span_in_sd * std_dev;
  const int samples = std::max(config.strikes_per_expiry, 3);

  std::vector<double> strikes;
  strikes.reserve(static_cast<std::size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    const double position = static_cast<double>(i) / static_cast<double>(samples - 1);
    const double k = -bound + position * 2.0 * bound;
    const double raw = forward * std::exp(k);
    const double strike =
        (config.strike_increment > 0.0)
            ? std::round(raw / config.strike_increment) * config.strike_increment
            : raw;
    if (strike > 0.0 && (strikes.empty() || strike > strikes.back())) {
      strikes.push_back(strike);
    }
  }
  return strikes;
}

}  // namespace

SimulationResult run_simulation(const SimulationConfig& config) {
  SimulationResult result{};

  std::mt19937_64 rng{config.seed};
  std::normal_distribution<double> gaussian{0.0, 1.0};
  std::uniform_real_distribution<double> uniform{0.0, 1.0};

  double spot = config.spot;
  double level = 0.0;
  double cash = 0.0;
  double hedge_units = 0.0;

  std::vector<SimPosition> positions;
  std::vector<PendingMarkout> pending;

  double edge_total = 0.0;
  double markout_total = 0.0;
  double informed_markout = 0.0;
  double uninformed_markout = 0.0;
  double informed_size = 0.0;
  double uninformed_size = 0.0;
  double traded_size = 0.0;

  const double dt = config.step_size;
  const double multiplier = config.quoting.risk.multiplier;

  for (int step = 0; step < config.steps; ++step) {
    const double now = static_cast<double>(step) * dt;

    const VolSurface surface = build_surface(config, now, spot, level);
    if (!surface.ok()) {
      break;
    }

    // Draw this step's market move up front, so informed counterparties can
    // act on the volatility shift before it happens. That is what makes them
    // informed rather than merely lucky.
    const double spot_shock = gaussian(rng);
    const double vol_shock = gaussian(rng);
    const double next_spot =
        spot * std::exp(-0.5 * config.realized_volatility * config.realized_volatility * dt +
                        config.realized_volatility * std::sqrt(dt) * spot_shock);
    const double next_level = level - config.vol_mean_reversion * level * dt +
                              config.vol_of_vol * std::sqrt(dt) * vol_shock;
    const double spot_move = next_spot - spot;
    const double level_move = next_level - level;

    PositionBook book = build_book(positions, now);
    const PortfolioRisk risk_before = compute_risk(book, surface, config.quoting.risk);
    const double value_before =
        risk_before.value + hedge_units * spot + cash;

    // --- quoting and flow -------------------------------------------------
    std::vector<ContractId> ladder;
    for (const SurfaceSlice& slice : surface.slices) {
      for (const double strike : strike_ladder(config, slice.forward, slice.time, level)) {
        ladder.push_back(ContractId{
            .time = slice.time,
            .strike = strike,
            .type = out_of_the_money_type(slice.forward, strike)});
      }
    }
    if (ladder.empty()) {
      break;
    }

    const std::vector<Quote> quotes =
        quote_ladder(ladder, surface, risk_before, config.quoting);

    for (int order = 0; order < config.orders_per_step; ++order) {
      const auto index = static_cast<std::size_t>(
          uniform(rng) * static_cast<double>(quotes.size())) % quotes.size();
      const Quote& quote = quotes[index];
      if (!quote.ok()) {
        ++result.orders_declined;
        continue;
      }

      const bool informed = uniform(rng) < config.informed_fraction;
      // An informed trader buys volatility when it is about to rise. An
      // uninformed one picks a side at random.
      const bool trader_buys = informed ? (level_move > 0.0) : (uniform(rng) < 0.5);

      const double available = trader_buys ? quote.ask_size : quote.bid_size;
      if (available <= 0.0) {
        ++result.orders_declined;
        continue;
      }

      const double size = std::fmin(config.order_size, available);
      const double trade_price = trader_buys ? quote.ask : quote.bid;
      // The maker takes the other side of whatever the trader does.
      const double maker_sign = trader_buys ? 1.0 : -1.0;
      const double edge =
          maker_sign * (trade_price - quote.theoretical_price) * size * multiplier;

      edge_total += edge;
      cash += maker_sign * trade_price * size * multiplier;
      traded_size += size;
      ++result.trades;

      const double expiry = now + quote.contract.time;
      const auto existing = std::find_if(
          positions.begin(), positions.end(), [&](const SimPosition& position) {
            return position.type == quote.contract.type &&
                   position.strike == quote.contract.strike &&
                   std::fabs(position.expiry - expiry) < 1e-9;
          });
      if (existing == positions.end()) {
        positions.push_back(SimPosition{.expiry = expiry,
                                        .strike = quote.contract.strike,
                                        .type = quote.contract.type,
                                        .quantity = -maker_sign * size});
      } else {
        existing->quantity += -maker_sign * size;
      }

      pending.push_back(PendingMarkout{.due_step = step + config.markout_steps,
                                       .expiry = expiry,
                                       .strike = quote.contract.strike,
                                       .type = quote.contract.type,
                                       .size = size,
                                       .maker_sign = maker_sign,
                                       .trade_price = trade_price,
                                       .theo_at_trade = quote.theoretical_price,
                                       .informed = informed});
      if (informed) {
        informed_size += size;
        ++result.adverse.informed_trades;
      } else {
        uninformed_size += size;
      }
    }

    // --- hedge ------------------------------------------------------------
    book = build_book(positions, now);
    const PortfolioRisk risk_after = compute_risk(book, surface, config.quoting.risk);
    result.peak_gross_vega = std::fmax(result.peak_gross_vega, risk_after.gross_vega);

    double step_hedge_cost = 0.0;
    double net_delta = risk_after.delta + hedge_units;
    if (std::fabs(net_delta) > config.hedge_band) {
      const double trade = -net_delta;
      step_hedge_cost = std::fabs(trade) * spot * config.hedge_cost_bps / 10000.0;
      cash -= trade * spot + step_hedge_cost;
      hedge_units += trade;
      result.hedge_turnover += std::fabs(trade) * spot;
      ++result.hedges;
      net_delta = risk_after.delta + hedge_units;
    }

    // Contracts in their final step are separated out before any greek is
    // read. Their gamma is diverging, so including them in the Taylor terms
    // would swamp the decomposition; their P&L is instead booked exactly, as
    // settlement value less last mark.
    const double next_now = now + dt;
    std::vector<SimPosition> expiring;
    std::vector<SimPosition> surviving;
    for (const SimPosition& position : positions) {
      if (position.expiry - next_now <= kMinimumRemainingTime) {
        expiring.push_back(position);
      } else {
        surviving.push_back(position);
      }
    }

    const PortfolioRisk risk_carried =
        compute_risk(build_book(surviving, now), surface, config.quoting.risk);
    const double expiring_mark =
        compute_risk(build_book(expiring, now), surface, config.quoting.risk).value;

    // --- advance the market ----------------------------------------------
    spot = next_spot;
    level = next_level;

    const VolSurface next_surface = build_surface(config, next_now, spot, level);

    double settled_cash = 0.0;
    for (const SimPosition& position : expiring) {
      const double payoff = (position.type == OptionType::Call)
                                ? std::fmax(spot - position.strike, 0.0)
                                : std::fmax(position.strike - spot, 0.0);
      settled_cash += position.quantity * multiplier * payoff;
    }
    cash += settled_cash;
    positions = std::move(surviving);

    const double settlement_term = settled_cash - expiring_mark;

    double value_after = hedge_units * spot + cash;
    if (next_surface.ok()) {
      const PositionBook next_book = build_book(positions, next_now);
      value_after +=
          compute_risk(next_book, next_surface, config.quoting.risk).value;
    }

    // --- attribute --------------------------------------------------------
    const double total = value_after - value_before;
    const double carried_delta = risk_carried.delta + hedge_units;
    const double delta_term = carried_delta * spot_move;
    const double gamma_term = 0.5 * risk_carried.gamma * spot_move * spot_move;
    const double vega_term = risk_carried.vega * level_move;
    const double theta_term = risk_carried.theta * dt;

    PnlAttribution step_pnl{};
    step_pnl.edge = edge_total;
    step_pnl.hedge_cost = step_hedge_cost;
    step_pnl.delta = delta_term;
    step_pnl.gamma = gamma_term;
    step_pnl.vega = vega_term;
    step_pnl.theta = theta_term;
    step_pnl.settlement = settlement_term;
    step_pnl.total = total;
    step_pnl.unexplained = total - step_pnl.explained();
    result.step_attribution.push_back(step_pnl);

    result.attribution.edge += edge_total;
    edge_total = 0.0;
    result.attribution.hedge_cost += step_hedge_cost;
    result.attribution.delta += delta_term;
    result.attribution.gamma += gamma_term;
    result.attribution.vega += vega_term;
    result.attribution.theta += theta_term;
    result.attribution.settlement += settlement_term;
    result.attribution.total += total;

    result.equity_curve.push_back(value_after);
    ++result.steps_run;

    // --- markouts ---------------------------------------------------------
    if (next_surface.ok()) {
      for (auto entry = pending.begin(); entry != pending.end();) {
        if (entry->due_step > step) {
          ++entry;
          continue;
        }
        const double remaining = entry->expiry - next_now;
        if (remaining <= kMinimumRemainingTime) {
          entry = pending.erase(entry);
          continue;
        }

        const double forward = next_surface.forward_at(remaining);
        const double log_moneyness = std::log(entry->strike / forward);
        const ForwardInputs in{
            .forward = forward,
            .strike = entry->strike,
            .discount_factor = next_surface.discount_factor_at(remaining),
            .volatility = next_surface.volatility_at(remaining, log_moneyness),
            .time = remaining};
        const double theo_now = price(in, entry->type);

        // What the trade was actually worth once fair value had moved:
        // positive means the maker kept money, negative means it was picked
        // off for more than the spread it charged.
        const double markout =
            entry->maker_sign * (entry->trade_price - theo_now) * entry->size;
        markout_total += markout;
        if (entry->informed) {
          informed_markout += markout;
        } else {
          uninformed_markout += markout;
        }
        entry = pending.erase(entry);
      }
    }
  }

  result.attribution.unexplained =
      result.attribution.total - result.attribution.explained();

  result.final_spot = spot;
  result.final_vol_level = level;

  result.adverse.trades = result.trades;
  const double measured = informed_size + uninformed_size;
  if (measured > 0.0) {
    result.adverse.edge_per_contract =
        result.attribution.edge / (traded_size * multiplier);
    result.adverse.markout_per_contract = markout_total / measured;
    if (std::fabs(result.adverse.edge_per_contract) > 0.0) {
      result.adverse.retained_fraction =
          result.adverse.markout_per_contract / result.adverse.edge_per_contract;
    }
  }
  if (informed_size > 0.0) {
    result.adverse.informed_markout_per_contract = informed_markout / informed_size;
  }
  if (uninformed_size > 0.0) {
    result.adverse.uninformed_markout_per_contract = uninformed_markout / uninformed_size;
  }

  return result;
}

}  // namespace skewdesk
