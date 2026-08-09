#pragma once

#include "skewdesk/chain.hpp"
#include "skewdesk/portfolio.hpp"
#include "skewdesk/quoting.hpp"
#include "skewdesk/surface.hpp"

#include <cstdint>
#include <vector>

namespace skewdesk {

// Where a market maker's money actually came from over a period.
//
// The identity being tested is the first-order expansion of the book's value
// over each step, with the position carried through the move:
//
//   dP  =  edge  -  hedge cost
//        +  delta_net * dS  +  0.5 * gamma * dS^2
//        +  vega * d(vol level)  +  theta * dt
//        +  unexplained
//
// `edge` is booked the instant a trade happens: the difference between where
// the maker transacted and where its own model said fair value was. It is
// gross revenue, and it is always positive at trade time. Everything after it
// is the cost of holding what the trade left behind.
//
// `unexplained` is the residual, and reporting it is the whole point. An
// attribution that does not show what it failed to explain is not an
// attribution -- it is a set of numbers that happen to sum to the answer
// because one of them absorbed the difference silently. Here that term is
// named, so the reader can judge whether the decomposition is describing the
// P&L or merely tiling it.
struct PnlAttribution {
  double edge{};
  double delta{};
  double gamma{};
  double vega{};
  double theta{};
  double hedge_cost{};
  // P&L of contracts that expired during the step, booked as the difference
  // between their settlement value and their last mark.
  //
  // Settlement gets its own term because the expansion above cannot describe
  // it. Gamma diverges as time to expiry goes to zero, so a contract in its
  // final step contributes a 0.5 * gamma * dS^2 of essentially unbounded size
  // against a realized P&L that is simply the payoff. Left in the Taylor
  // terms it does not merely add noise -- it swamps the entire decomposition,
  // with the residual quietly absorbing an equal and opposite error. Expiring
  // contracts are therefore excluded from the greek terms entirely and their
  // realized P&L booked here exactly.
  double settlement{};
  double unexplained{};
  double total{};

  [[nodiscard]] double explained() const noexcept {
    return edge - hedge_cost + delta + gamma + vega + theta + settlement;
  }
};

// How much of the quoted edge actually survives contact with the counterparty.
//
// A maker that captures half a volatility point of edge on every trade and
// then watches fair value move against it by four tenths of a point has kept
// twenty percent of what its spread appeared to earn. That gap is adverse
// selection, and it is invisible in gross revenue.
//
// Measured by marking every trade against the model a fixed number of steps
// later. Informed and uninformed flow are reported separately, because the
// average over both conceals the thing worth knowing: whether the losses are
// concentrated in the counterparties who knew something.
struct AdverseSelection {
  int trades{};
  int informed_trades{};
  double edge_per_contract{};
  double markout_per_contract{};
  // markout / edge. One means the edge was kept in full; below zero means the
  // trades lost more than the spread earned.
  double retained_fraction{};
  double informed_markout_per_contract{};
  double uninformed_markout_per_contract{};
};

struct SimulationConfig {
  // The market's true volatility surface shape. The maker is assumed to know
  // it: quotes are struck off this surface, so the attribution isolates
  // market-making mechanics -- spread, inventory, hedging, adverse selection
  // -- from model error, which M4 already measures separately.
  SkewParameters skew{};

  double spot{4500.0};
  double rate{0.042};
  double dividend{0.013};

  // Absolute maturities, measured from the start of the run. They shrink as
  // the simulation advances, so time decay is real rather than assumed.
  std::vector<double> expiries{0.08, 0.25, 0.5, 1.0};
  int strikes_per_expiry{9};
  double strike_span_in_sd{2.0};
  double strike_increment{25.0};

  int steps{120};
  double step_size{1.0 / 252.0};

  double realized_volatility{0.16};

  // The surface moves by a mean-reverting parallel shift in volatility. A
  // parallel shift is a simplification -- real surfaces twist as well as
  // translate -- but it is the shift that vega is defined against, which keeps
  // the vega term in the attribution honest rather than approximate.
  //
  // These two numbers have to be chosen together, and the choice is not
  // cosmetic. An Ornstein-Uhlenbeck level has stationary standard deviation
  // vol_of_vol / sqrt(2 * mean_reversion); at 0.60 and 3.0 that is 24
  // volatility points on a 19-volatility surface, so the level regularly
  // drives implied volatility to zero. There the surface stops being a
  // volatility surface at all: d1 runs past 20 standard deviations, gamma and
  // vega underflow to around 1e-100, and the P&L attribution collapses because
  // its greeks have all silently vanished. The defaults below give a
  // stationary spread of roughly three volatility points, which is both
  // realistic and safely away from that boundary.
  double vol_of_vol{0.08};
  double vol_mean_reversion{3.0};

  // Hard floor under the market's implied volatility, so an extreme draw
  // degrades the realism of the run rather than its arithmetic.
  double minimum_volatility{0.02};

  // Fraction of arriving orders that know the sign of the volatility move
  // about to happen and trade accordingly. Without informed flow there is no
  // adverse selection to measure: a maker facing purely random counterparties
  // keeps its whole spread, and any markout is sampling noise.
  double informed_fraction{0.30};
  int orders_per_step{6};
  double order_size{5.0};

  // Delta is re-flattened only when it drifts outside this band, in underlying
  // units. Hedging continuously would eliminate the delta term and most of the
  // hedge cost, and no desk does it.
  double hedge_band{2000.0};
  double hedge_cost_bps{0.5};

  int markout_steps{5};

  QuoteSettings quoting{};
  SviFitSettings fit{};
  std::uint64_t seed{20260808};
};

struct SimulationResult {
  PnlAttribution attribution{};
  AdverseSelection adverse{};
  // Per-step decomposition, so the residual can be located in time rather than
  // only totalled. Attribution quality is not uniform across a run: it decays
  // wherever the first-order expansion stops describing the position.
  std::vector<PnlAttribution> step_attribution{};
  std::vector<double> equity_curve{};
  int steps_run{};
  int trades{};
  int orders_declined{};
  int hedges{};
  double hedge_turnover{};
  double final_spot{};
  double final_vol_level{};
  double peak_gross_vega{};
};

[[nodiscard]] SimulationResult run_simulation(const SimulationConfig& config);

}  // namespace skewdesk
