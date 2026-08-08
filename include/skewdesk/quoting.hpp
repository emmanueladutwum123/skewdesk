#pragma once

#include "skewdesk/portfolio.hpp"
#include "skewdesk/surface.hpp"

#include <span>
#include <vector>

namespace skewdesk {

struct QuoteSettings {
  // Everything about the width is expressed in volatility points, because
  // that is the unit a desk actually quotes in. Converting to price at the end
  // via the option's own vega is what makes wing markets narrow in price and
  // near-the-money markets wide, automatically.
  double base_half_width_vol_points{0.005};

  // Extra half-width carried at full inventory utilisation. Widening as the
  // book fills is how a maker charges more for risk it is less able to take.
  double inventory_width_vol_points{0.010};

  // Scales the slice's own fit residual into quoted width. This is the
  // surface telling the quoter how much to trust it: an expiry whose SVI fit
  // left large residuals is one where theo is genuinely uncertain, and
  // quoting it as tightly as a clean expiry would be quoting confidence that
  // does not exist.
  double uncertainty_width_multiplier{1.0};

  // Largest shift of the quote's centre away from theo at full utilisation.
  double max_skew_vol_points{0.015};

  // Risk budget for one tenor x log-moneyness cell of the vega grid.
  double bucket_vega_limit{2.0e7};

  // Floor on the quoted half-width in price terms. Short-dated and deep-wing
  // options have almost no vega, so a purely volatility-based width collapses
  // to nothing in price terms exactly where the maker is most exposed to
  // being picked off. The floor is what stops that.
  double minimum_half_width{0.05};

  double base_size{50.0};
  double minimum_size{1.0};

  // Must match the settings the PortfolioRisk was computed with, or the
  // bucket lookup would read the wrong cell.
  RiskSettings risk{};
};

enum class QuoteStatus {
  Success,
  // No usable surface, or the contract cannot be priced against it.
  NotPriceable,
  // The supplied risk was bucketed differently from these settings, so the
  // inventory lookup would read the wrong cell. Reported rather than guessed
  // at, since a silently mismatched bucket produces a confident wrong skew.
  RiskBucketMismatch,
};

struct Quote {
  ContractId contract{};

  double theoretical_volatility{};
  double quoted_mid_volatility{};
  double bid_volatility{};
  double ask_volatility{};

  double theoretical_price{};
  double bid{};
  double ask{};

  // Zero means that side is not being shown at all.
  double bid_size{};
  double ask_size{};

  // Signed inventory utilisation of the contract's vega bucket, clamped to
  // [-1, 1]. Positive means the book is already long volatility there.
  double inventory_utilisation{};

  QuoteStatus status{QuoteStatus::NotPriceable};

  [[nodiscard]] bool ok() const noexcept { return status == QuoteStatus::Success; }
  [[nodiscard]] bool two_sided() const noexcept { return bid_size > 0.0 && ask_size > 0.0; }
};

// Produces a two-sided market in one contract.
//
// The centre of the quote is shifted away from theo in the direction that
// makes risk-reducing trades more attractive than risk-adding ones: a book
// already long volatility in a bucket quotes that bucket lower, so it is more
// likely to be lifted than hit. This is the same idea as the reservation price
// in the Avellaneda-Stoikov market-making model, applied to vega inventory
// rather than to delta inventory -- the maker is not trying to predict where
// volatility goes, only to make the flow it attracts lean toward flat.
//
// At full utilisation the quoter stops showing the side that would add to the
// position, rather than merely pricing it badly. A market maker at its risk
// limit pulls the bid; it does not keep bidding at a silly price and hope.
//
// Risk is taken pre-computed rather than as a book, since a desk computes its
// risk once and then quotes hundreds of contracts against it.
[[nodiscard]] Quote make_quote(const ContractId& contract, const VolSurface& surface,
                               const PortfolioRisk& risk,
                               const QuoteSettings& settings = {});

[[nodiscard]] std::vector<Quote> quote_ladder(std::span<const ContractId> contracts,
                                              const VolSurface& surface,
                                              const PortfolioRisk& risk,
                                              const QuoteSettings& settings = {});

}  // namespace skewdesk
