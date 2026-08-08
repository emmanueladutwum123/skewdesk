#pragma once

#include "skewdesk/black_scholes.hpp"
#include "skewdesk/surface.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace skewdesk {

struct ContractId {
  double time{};
  double strike{};
  OptionType type{};
};

struct Position {
  ContractId contract{};
  // Signed, in contracts. Negative is short.
  double quantity{};
};

// A book of option positions, netted by contract.
//
// Adding to a contract already held nets the quantities, and a contract netted
// flat is removed outright rather than kept as a zero row -- a position of
// zero is not a position, and leaving it in inflates every count and every
// gross-risk figure that iterates the book.
class PositionBook {
 public:
  void add(const ContractId& contract, double quantity);
  void clear() noexcept;

  [[nodiscard]] std::span<const Position> positions() const noexcept { return positions_; }
  [[nodiscard]] std::size_t size() const noexcept { return positions_.size(); }
  [[nodiscard]] bool empty() const noexcept { return positions_.empty(); }
  [[nodiscard]] double quantity_of(const ContractId& contract) const noexcept;

 private:
  std::vector<Position> positions_;
};

// Bucket boundaries. Each vector holds the interior edges, so N edges produce
// N+1 buckets, half-open as [lower, upper).
struct RiskBucketConfig {
  std::vector<double> tenor_edges{1.0 / 12.0, 0.25, 0.5, 1.0, 2.0};
  std::vector<double> log_moneyness_edges{-0.10, -0.03, 0.03, 0.10};

  [[nodiscard]] std::size_t tenor_bucket_count() const noexcept {
    return tenor_edges.size() + 1;
  }
  [[nodiscard]] std::size_t moneyness_bucket_count() const noexcept {
    return log_moneyness_edges.size() + 1;
  }
};

struct RiskSettings {
  // Contract size. Index options are quoted per point but settle on a
  // multiplier, so risk figures are meaningless without it.
  double multiplier{100.0};
  RiskBucketConfig buckets{};
  // Maturity that term-weighted vega is expressed relative to. Short-dated
  // implied volatility moves far more than long-dated, so a vega point at
  // three weeks is not the same risk as one at two years; scaling by
  // sqrt(reference / T) puts them on a comparable footing. Three months is
  // the usual convention.
  double weighted_vega_reference_time{0.25};
};

// Vega broken out by where the risk actually sits.
//
// This is the point of the whole milestone. A book long 1,000 vega in the
// front month and short 1,000 vega in the two-year has zero net vega and an
// enormous position: it is short the term structure, and any differential move
// hurts. The same is true across strikes -- long downside vega against short
// upside vega is a skew position, not a flat one. A single aggregate number
// conceals both, which is why desks look at the grid.
struct VegaBuckets {
  std::vector<double> by_tenor{};
  std::vector<double> by_log_moneyness{};
  // Row-major, tenor-major: grid[tenor][moneyness].
  std::vector<std::vector<double>> grid{};
};

struct PortfolioRisk {
  double value{};
  double delta{};
  double gamma{};
  double vega{};
  double theta{};
  double weighted_vega{};
  // Sum of the absolute vega in each grid cell. Equal to |vega| only when all
  // the risk sits in one cell; the gap between this and |vega| is exactly the
  // offsetting exposure that a net figure hides.
  double gross_vega{};
  VegaBuckets vega_buckets{};
  int positions_priced{};
  int positions_skipped{};
};

[[nodiscard]] std::size_t bucket_index(double value, std::span<const double> edges) noexcept;

// Marks a book against a fitted surface and aggregates its risk.
//
// The greeks are Black-Scholes greeks evaluated at the surface's volatility,
// with the surface held fixed -- sticky-strike, in desk language. Delta
// therefore excludes the way implied volatility at a given strike tends to
// move when spot moves. That is a modelling choice, not an oversight: the
// alternative depends on a view about how the surface travels, and pretending
// otherwise would bury that assumption inside a number labelled "delta".
[[nodiscard]] PortfolioRisk compute_risk(const PositionBook& book, const VolSurface& surface,
                                         const RiskSettings& settings = {});

}  // namespace skewdesk
