#include "skewdesk/simulation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace skewdesk {
namespace {

SimulationConfig ShortRun() {
  SimulationConfig config{};
  config.steps = 60;
  config.expiries = {0.15, 0.5, 1.0};
  config.strikes_per_expiry = 7;
  return config;
}

TEST(Simulation, IsReproducibleFromItsSeed) {
  const SimulationConfig config = ShortRun();
  const SimulationResult first = run_simulation(config);
  const SimulationResult second = run_simulation(config);

  EXPECT_EQ(first.trades, second.trades);
  EXPECT_DOUBLE_EQ(first.attribution.total, second.attribution.total);
  EXPECT_DOUBLE_EQ(first.attribution.edge, second.attribution.edge);
  EXPECT_DOUBLE_EQ(first.final_spot, second.final_spot);

  SimulationConfig other = config;
  other.seed += 1;
  EXPECT_NE(run_simulation(other).attribution.total, first.attribution.total);
}

// The identity the whole attribution rests on. It holds by construction --
// `unexplained` is defined as the residual -- so this is really a guard
// against the bookkeeping drifting apart from the definition.
TEST(Simulation, AttributionComponentsReconcileToTotal) {
  const SimulationResult result = run_simulation(ShortRun());
  ASSERT_GT(result.steps_run, 0);

  const PnlAttribution& pnl = result.attribution;
  EXPECT_NEAR(pnl.explained() + pnl.unexplained, pnl.total, 1e-6);
}

// The more interesting question: is the decomposition actually describing the
// P&L, or merely tiling it? Measured against gross activity rather than net
// total, since a net total near zero would flatter any residual.
TEST(Simulation, UnexplainedResidualIsSmallRelativeToGrossAttribution) {
  const SimulationResult result = run_simulation(ShortRun());
  const PnlAttribution& pnl = result.attribution;

  const double gross = std::fabs(pnl.edge) + std::fabs(pnl.delta) +
                       std::fabs(pnl.gamma) + std::fabs(pnl.vega) +
                       std::fabs(pnl.theta) + std::fabs(pnl.hedge_cost) +
                       std::fabs(pnl.settlement);
  ASSERT_GT(gross, 0.0);
  // Measured between 1.3% and 5% across configurations; the bound is set with
  // headroom for floating-point trajectory differences between compilers, not
  // because the residual is expected to be large.
  EXPECT_LT(std::fabs(pnl.unexplained) / gross, 0.10)
      << "unexplained=" << pnl.unexplained << " gross=" << gross;
}

// Expiring contracts are excluded from the greek terms and booked separately,
// because gamma diverges as time to expiry goes to zero. Without that split
// the final step of an expiry contributes a nonsense second-order term that
// the residual silently absorbs -- measured at 87% of gross attribution
// before the carve-out, and under 5% after it.
TEST(Simulation, ExpiringContractsAreBookedAsSettlementNotAsGamma) {
  SimulationConfig config = ShortRun();
  config.expiries = {0.10, 0.5};
  config.steps = 80;
  ASSERT_GT(static_cast<double>(config.steps) * config.step_size, 0.10)
      << "premise: the run must outlive the front expiry";

  const SimulationResult result = run_simulation(config);
  ASSERT_GT(result.steps_run, 0);
  EXPECT_NE(result.attribution.settlement, 0.0) << "no expiry was ever settled";

  int settling_steps = 0;
  for (const PnlAttribution& step : result.step_attribution) {
    if (step.settlement != 0.0) {
      ++settling_steps;
    }
  }
  EXPECT_GT(settling_steps, 0);
}

// Edge is booked as the difference between where the maker transacted and its
// own theo, so it can only ever be positive at trade time. A negative total
// would mean the quoting engine crossed its own fair value.
TEST(Simulation, EdgeIsAlwaysCapturedInTheMakersFavour) {
  const SimulationResult result = run_simulation(ShortRun());
  ASSERT_GT(result.trades, 0);
  EXPECT_GT(result.attribution.edge, 0.0);
}

// The point of modelling informed flow at all. Counterparties who know which
// way volatility is about to move should mark out worse for the maker than
// counterparties picking a side at random.
//
// Averaged over several seeds rather than asserted on one. A single run's
// markout carries real sampling noise -- the effect holds in 8 of 8 seeds
// measured, but individual seeds vary by more than the gap itself, and a
// one-seed assertion would be testing the draw as much as the mechanism.
TEST(Simulation, InformedFlowMarksOutWorseThanUninformedFlow) {
  double informed_total = 0.0;
  double uninformed_total = 0.0;
  int runs = 0;

  for (std::uint64_t offset = 1; offset <= 5; ++offset) {
    SimulationConfig config = ShortRun();
    config.steps = 250;
    config.informed_fraction = 0.5;
    config.seed += offset;

    const SimulationResult result = run_simulation(config);
    ASSERT_GT(result.adverse.informed_trades, 20);
    ASSERT_LT(result.adverse.informed_trades, result.trades);

    informed_total += result.adverse.informed_markout_per_contract;
    uninformed_total += result.adverse.uninformed_markout_per_contract;
    ++runs;
  }

  const double informed = informed_total / runs;
  const double uninformed = uninformed_total / runs;
  EXPECT_LT(informed, uninformed) << "informed=" << informed
                                  << " uninformed=" << uninformed;
}

// With no informed counterparties there is nothing to be adversely selected
// by, so the maker should retain most of its quoted edge. This is the control
// that gives the informed-flow result its meaning. Measured at 0.85.
TEST(Simulation, PurelyUninformedFlowRetainsMostOfTheEdge) {
  SimulationConfig config = ShortRun();
  config.steps = 250;
  config.informed_fraction = 0.0;

  const SimulationResult result = run_simulation(config);
  ASSERT_GT(result.trades, 50);
  EXPECT_EQ(result.adverse.informed_trades, 0);
  EXPECT_GT(result.adverse.retained_fraction, 0.5)
      << "retained=" << result.adverse.retained_fraction;
}

// Measured at 0.85 retained against fully uninformed flow and 0.25 against
// three-quarters informed flow. The endpoints are compared rather than every
// intermediate point, because a single run's retained fraction carries real
// sampling noise and asserting strict monotonicity across it would be
// asserting the noise.
TEST(Simulation, InformedFlowErodesRetainedEdge) {
  SimulationConfig benign = ShortRun();
  benign.steps = 250;
  benign.informed_fraction = 0.0;

  SimulationConfig toxic = benign;
  toxic.informed_fraction = 0.75;

  const SimulationResult clean = run_simulation(benign);
  const SimulationResult picked_off = run_simulation(toxic);

  ASSERT_GT(clean.trades, 50);
  ASSERT_GT(picked_off.adverse.informed_trades, 100);

  EXPECT_LT(picked_off.adverse.retained_fraction,
            clean.adverse.retained_fraction - 0.2)
      << "clean=" << clean.adverse.retained_fraction
      << " toxic=" << picked_off.adverse.retained_fraction;
}

// Hedging in a band rather than continuously is what leaves a delta term in
// the attribution at all; a tight band should shrink it and cost more.
TEST(Simulation, TighterHedgeBandTradesDeltaRiskForCost) {
  SimulationConfig loose = ShortRun();
  loose.steps = 150;
  loose.hedge_band = 1.0e6;

  SimulationConfig tight = loose;
  tight.hedge_band = 100.0;

  const SimulationResult loose_result = run_simulation(loose);
  const SimulationResult tight_result = run_simulation(tight);

  EXPECT_GT(tight_result.hedges, loose_result.hedges);
  EXPECT_GT(tight_result.attribution.hedge_cost, loose_result.attribution.hedge_cost);
  EXPECT_LT(std::fabs(tight_result.attribution.delta),
            std::fabs(loose_result.attribution.delta));
}

TEST(Simulation, NoHedgingLeavesTheHedgeCostAtZero) {
  SimulationConfig config = ShortRun();
  config.hedge_band = 1.0e12;

  const SimulationResult result = run_simulation(config);
  EXPECT_EQ(result.hedges, 0);
  EXPECT_DOUBLE_EQ(result.attribution.hedge_cost, 0.0);
  EXPECT_DOUBLE_EQ(result.hedge_turnover, 0.0);
}

// Gamma and theta are two views of the same exposure: a book long gamma pays
// for it in decay, and one short gamma is paid to carry the risk. So the two
// terms should oppose each other step by step.
//
// Deliberately checked per step rather than on the run totals. The totals are
// sums over steps whose signs vary, and gamma is additionally weighted by the
// size of each move, so a run can perfectly well finish with both totals
// positive -- measured at 78% to 92% of steps opposing, with aggregate signs
// agreeing in some runs. Asserting it on the totals would be asserting
// something that is not true.
TEST(Simulation, GammaAndThetaOpposeEachOtherStepByStep) {
  SimulationConfig config = ShortRun();
  config.steps = 250;
  const SimulationResult result = run_simulation(config);

  int opposing = 0;
  int counted = 0;
  for (const PnlAttribution& step : result.step_attribution) {
    if (step.gamma == 0.0 || step.theta == 0.0) {
      continue;
    }
    if ((step.gamma > 0.0) != (step.theta > 0.0)) {
      ++opposing;
    }
    ++counted;
  }

  ASSERT_GT(counted, 50);
  EXPECT_GT(static_cast<double>(opposing) / counted, 0.65)
      << opposing << " of " << counted << " steps opposed";
}

TEST(Simulation, QuietMarketsLeaveLittleForGammaAndVegaToExplain) {
  SimulationConfig config = ShortRun();
  config.realized_volatility = 1e-6;
  config.vol_of_vol = 1e-6;
  config.steps = 80;

  const SimulationResult result = run_simulation(config);
  ASSERT_GT(result.trades, 0);

  EXPECT_LT(std::fabs(result.attribution.gamma), std::fabs(result.attribution.theta));
  EXPECT_LT(std::fabs(result.attribution.vega), std::fabs(result.attribution.edge));
}

TEST(Simulation, RunsToCompletionAndRecordsAnEquityCurve) {
  const SimulationConfig config = ShortRun();
  const SimulationResult result = run_simulation(config);

  EXPECT_EQ(result.steps_run, config.steps);
  EXPECT_EQ(static_cast<int>(result.equity_curve.size()), config.steps);
  EXPECT_GT(result.peak_gross_vega, 0.0);
  EXPECT_TRUE(std::isfinite(result.final_spot));
  EXPECT_GT(result.final_spot, 0.0);
  for (const double point : result.equity_curve) {
    EXPECT_TRUE(std::isfinite(point));
  }
}

// Contracts run past their expiry during the default configuration, so the
// settlement path is exercised rather than merely present.
TEST(Simulation, SurvivesExpiriesRollingOffDuringTheRun) {
  SimulationConfig config = ShortRun();
  config.expiries = {0.05, 0.25};
  config.steps = 100;
  config.step_size = 1.0 / 252.0;
  ASSERT_GT(static_cast<double>(config.steps) * config.step_size, 0.05)
      << "premise: the run must outlive the front expiry";

  const SimulationResult result = run_simulation(config);
  EXPECT_GT(result.steps_run, 0);
  EXPECT_TRUE(std::isfinite(result.attribution.total));
  EXPECT_TRUE(std::isfinite(result.attribution.unexplained));
}

TEST(Simulation, DecliningOrdersIsCountedNotSilentlyDropped) {
  SimulationConfig config = ShortRun();
  config.steps = 200;
  // A tiny bucket limit means the quoter reaches its limit quickly and starts
  // withdrawing the adding side.
  config.quoting.bucket_vega_limit = 1.0e4;

  const SimulationResult result = run_simulation(config);
  EXPECT_GT(result.orders_declined, 0);
  EXPECT_EQ(result.trades + result.orders_declined,
            config.orders_per_step * result.steps_run);
}

}  // namespace
}  // namespace skewdesk
