#include "skewdesk/quoting.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <initializer_list>
#include <vector>

namespace skewdesk {
namespace {

constexpr double kSpot = 4500.0;

SurfaceSlice FlatSlice(double time, double volatility, double rmse = 0.0) {
  SurfaceSlice slice{};
  slice.time = time;
  slice.forward = kSpot * std::exp(0.042 * time);
  slice.discount_factor = std::exp(-0.042 * time);
  slice.k_low = -0.5;
  slice.k_high = 0.5;
  slice.fit.time = time;
  slice.fit.rmse = rmse;
  slice.fit.status = SviFitStatus::Success;
  slice.fit.parameters = SviParameters{
      .a = volatility * volatility * time, .b = 0.0, .rho = 0.0, .m = 0.0, .sigma = 0.2};
  return slice;
}

VolSurface FlatSurface(double volatility = 0.20, double rmse = 0.0) {
  VolSurface surface{};
  surface.spot = kSpot;
  surface.slices = {FlatSlice(0.08, volatility, rmse), FlatSlice(0.25, volatility, rmse),
                    FlatSlice(1.0, volatility, rmse), FlatSlice(2.0, volatility, rmse)};
  surface.status = SurfaceStatus::Success;
  return surface;
}

PortfolioRisk FlatRisk(const RiskSettings& settings = {}) {
  PortfolioRisk risk{};
  const std::size_t tenors = settings.buckets.tenor_bucket_count();
  const std::size_t moneyness = settings.buckets.moneyness_bucket_count();
  risk.vega_buckets.by_tenor.assign(tenors, 0.0);
  risk.vega_buckets.by_log_moneyness.assign(moneyness, 0.0);
  risk.vega_buckets.grid.assign(tenors, std::vector<double>(moneyness, 0.0));
  return risk;
}

// Places a vega exposure into whichever bucket the given contract falls in.
PortfolioRisk RiskWithBucketVega(const ContractId& contract, const VolSurface& surface,
                                 double vega, const RiskSettings& settings = {}) {
  PortfolioRisk risk = FlatRisk(settings);
  const double log_moneyness =
      std::log(contract.strike / surface.forward_at(contract.time));
  const std::size_t tenor = bucket_index(contract.time, settings.buckets.tenor_edges);
  const std::size_t moneyness =
      bucket_index(log_moneyness, settings.buckets.log_moneyness_edges);
  risk.vega_buckets.grid[tenor][moneyness] = vega;
  risk.vega_buckets.by_tenor[tenor] = vega;
  risk.vega_buckets.by_log_moneyness[moneyness] = vega;
  risk.vega = vega;
  risk.gross_vega = std::fabs(vega);
  return risk;
}

ContractId AtTheMoney(double time = 0.25) {
  return ContractId{.time = time, .strike = 4550.0, .type = OptionType::Call};
}

TEST(Quoting, FlatBookQuotesSymmetricallyAroundTheo) {
  const VolSurface surface = FlatSurface();
  const Quote quote = make_quote(AtTheMoney(), surface, FlatRisk());

  ASSERT_TRUE(quote.ok());
  EXPECT_DOUBLE_EQ(quote.quoted_mid_volatility, quote.theoretical_volatility);
  EXPECT_DOUBLE_EQ(quote.inventory_utilisation, 0.0);

  const QuoteSettings settings{};
  EXPECT_NEAR(quote.ask_volatility - quote.theoretical_volatility,
              settings.base_half_width_vol_points, 1e-12);
  EXPECT_NEAR(quote.theoretical_volatility - quote.bid_volatility,
              settings.base_half_width_vol_points, 1e-12);
  EXPECT_TRUE(quote.two_sided());
}

// The core behaviour: a book already long volatility in a bucket shows that
// bucket lower on both sides, so the flow it attracts sells to the maker.
TEST(Quoting, LongInventorySkewsBothSidesDown) {
  const VolSurface surface = FlatSurface();
  const ContractId contract = AtTheMoney();
  const QuoteSettings settings{};

  const Quote flat = make_quote(contract, surface, FlatRisk(), settings);
  const Quote when_long = make_quote(
      contract, surface,
      RiskWithBucketVega(contract, surface, 0.5 * settings.bucket_vega_limit), settings);

  ASSERT_TRUE(flat.ok());
  ASSERT_TRUE(when_long.ok());

  EXPECT_LT(when_long.quoted_mid_volatility, flat.quoted_mid_volatility);
  EXPECT_LT(when_long.bid, flat.bid);
  EXPECT_LT(when_long.ask, flat.ask);
  EXPECT_NEAR(when_long.inventory_utilisation, 0.5, 1e-12);
  EXPECT_NEAR(flat.theoretical_volatility - when_long.quoted_mid_volatility,
              0.5 * settings.max_skew_vol_points, 1e-12);
}

TEST(Quoting, ShortInventorySkewsBothSidesUp) {
  const VolSurface surface = FlatSurface();
  const ContractId contract = AtTheMoney();
  const QuoteSettings settings{};

  const Quote flat = make_quote(contract, surface, FlatRisk(), settings);
  const Quote when_short = make_quote(
      contract, surface,
      RiskWithBucketVega(contract, surface, -0.5 * settings.bucket_vega_limit), settings);

  ASSERT_TRUE(when_short.ok());
  EXPECT_GT(when_short.quoted_mid_volatility, flat.quoted_mid_volatility);
  EXPECT_GT(when_short.bid, flat.bid);
  EXPECT_GT(when_short.ask, flat.ask);
}

TEST(Quoting, SkewAndUtilisationSaturateBeyondTheLimit) {
  const VolSurface surface = FlatSurface();
  const ContractId contract = AtTheMoney();
  const QuoteSettings settings{};

  const Quote at_limit = make_quote(
      contract, surface, RiskWithBucketVega(contract, surface, settings.bucket_vega_limit),
      settings);
  const Quote far_beyond = make_quote(
      contract, surface,
      RiskWithBucketVega(contract, surface, 25.0 * settings.bucket_vega_limit), settings);

  ASSERT_TRUE(at_limit.ok());
  ASSERT_TRUE(far_beyond.ok());
  EXPECT_DOUBLE_EQ(at_limit.inventory_utilisation, 1.0);
  EXPECT_DOUBLE_EQ(far_beyond.inventory_utilisation, 1.0);
  EXPECT_DOUBLE_EQ(at_limit.quoted_mid_volatility, far_beyond.quoted_mid_volatility);
}

TEST(Quoting, WidthGrowsWithInventoryUtilisation) {
  const VolSurface surface = FlatSurface();
  const ContractId contract = AtTheMoney();
  const QuoteSettings settings{};

  double previous_width = 0.0;
  for (const double fraction : {0.0, 0.25, 0.5, 1.0}) {
    const Quote quote = make_quote(
        contract, surface,
        RiskWithBucketVega(contract, surface, fraction * settings.bucket_vega_limit),
        settings);
    ASSERT_TRUE(quote.ok());
    const double width = quote.ask_volatility - quote.bid_volatility;
    EXPECT_GT(width, previous_width) << "fraction=" << fraction;
    previous_width = width;
  }
}

// The surface telling the quoter how much to trust it: a slice that fitted
// badly is quoted wider than one that fitted cleanly.
TEST(Quoting, WidthGrowsWithSliceFitUncertainty) {
  const ContractId contract = AtTheMoney(1.0);

  const Quote clean = make_quote(contract, FlatSurface(0.20, 0.0), FlatRisk());
  const Quote noisy = make_quote(contract, FlatSurface(0.20, 5e-3), FlatRisk());

  ASSERT_TRUE(clean.ok());
  ASSERT_TRUE(noisy.ok());
  EXPECT_GT(noisy.ask_volatility - noisy.bid_volatility,
            clean.ask_volatility - clean.bid_volatility);
  // Both still centred on theo -- uncertainty widens, it does not skew.
  EXPECT_DOUBLE_EQ(noisy.quoted_mid_volatility, clean.quoted_mid_volatility);
}

// A width set purely in volatility collapses in price terms where vega is
// negligible, which is exactly where a maker is most exposed to being picked
// off. The floor is what prevents a one-tick market on a lottery ticket.
TEST(Quoting, MinimumPriceWidthFloorsNegligibleVegaContracts) {
  const VolSurface surface = FlatSurface();
  const QuoteSettings settings{};

  // Deep in the money and short dated: vega is negligible, so the volatility
  // width implies almost no price width, but the option is worth a great deal
  // and there is room for the floor on both sides.
  const ContractId deep_itm{.time = 0.08, .strike = 3000.0, .type = OptionType::Call};
  const Quote itm = make_quote(deep_itm, surface, FlatRisk(), settings);

  ASSERT_TRUE(itm.ok());
  ASSERT_GT(itm.theoretical_price, 1000.0) << "premise: this option is deep in the money";
  EXPECT_NEAR(itm.ask - itm.bid, 2.0 * settings.minimum_half_width, 1e-9);
}

// The other branch of the floor. A worthless deep-wing option cannot be bid
// below zero, so the two-sided floor is impossible and the maker shows a zero
// bid against a small offer -- which is exactly what a real market in a
// lottery ticket looks like.
TEST(Quoting, WorthlessWingsQuoteAZeroBidAgainstAnOffer) {
  const VolSurface surface = FlatSurface();
  const QuoteSettings settings{};

  const ContractId deep_wing{.time = 0.08, .strike = 9000.0, .type = OptionType::Call};
  const Quote quote = make_quote(deep_wing, surface, FlatRisk(), settings);

  ASSERT_TRUE(quote.ok());
  ASSERT_LT(quote.theoretical_price, 1e-6) << "premise: this option is worthless";

  EXPECT_DOUBLE_EQ(quote.bid, 0.0);
  EXPECT_NEAR(quote.ask, settings.minimum_half_width, 1e-6);
  EXPECT_LT(quote.bid, quote.ask);
}

// A maker at its risk limit pulls the side that would add to the position
// rather than continuing to show it at a silly price.
TEST(Quoting, WithdrawsTheAddingSideAtTheLimit) {
  const VolSurface surface = FlatSurface();
  const ContractId contract = AtTheMoney();
  const QuoteSettings settings{};

  const Quote max_long = make_quote(
      contract, surface, RiskWithBucketVega(contract, surface, settings.bucket_vega_limit),
      settings);
  ASSERT_TRUE(max_long.ok());
  EXPECT_DOUBLE_EQ(max_long.bid_size, 0.0) << "should stop bidding when maximally long";
  EXPECT_GT(max_long.ask_size, 0.0) << "should still offer, to reduce";
  EXPECT_FALSE(max_long.two_sided());

  const Quote max_short = make_quote(
      contract, surface, RiskWithBucketVega(contract, surface, -settings.bucket_vega_limit),
      settings);
  ASSERT_TRUE(max_short.ok());
  EXPECT_DOUBLE_EQ(max_short.ask_size, 0.0);
  EXPECT_GT(max_short.bid_size, 0.0);
}

TEST(Quoting, SizeTapersOnTheAddingSideOnly) {
  const VolSurface surface = FlatSurface();
  const ContractId contract = AtTheMoney();
  const QuoteSettings settings{};

  const Quote flat = make_quote(contract, surface, FlatRisk(), settings);
  const Quote half_long = make_quote(
      contract, surface,
      RiskWithBucketVega(contract, surface, 0.5 * settings.bucket_vega_limit), settings);

  EXPECT_LT(half_long.bid_size, flat.bid_size);
  EXPECT_DOUBLE_EQ(half_long.ask_size, flat.ask_size)
      << "the reducing side stays at full size";
}

TEST(Quoting, BidIsNeverNegativeAndAlwaysBelowAsk) {
  const VolSurface surface = FlatSurface();

  for (const double strike : {2000.0, 3500.0, 4500.0, 6000.0, 12000.0}) {
    for (const double time : {0.02, 0.25, 2.0}) {
      for (const OptionType type : {OptionType::Call, OptionType::Put}) {
        const ContractId contract{.time = time, .strike = strike, .type = type};
        const Quote quote = make_quote(contract, surface, FlatRisk());
        ASSERT_TRUE(quote.ok()) << "K=" << strike << " T=" << time;
        EXPECT_GE(quote.bid, 0.0) << "K=" << strike << " T=" << time;
        EXPECT_LT(quote.bid, quote.ask) << "K=" << strike << " T=" << time;
      }
    }
  }
}

TEST(Quoting, MismatchedRiskBucketsAreReportedNotGuessedAt) {
  const VolSurface surface = FlatSurface();

  RiskSettings other{};
  other.buckets.tenor_edges = {0.5};
  other.buckets.log_moneyness_edges = {0.0};

  const Quote quote = make_quote(AtTheMoney(), surface, FlatRisk(other), QuoteSettings{});
  EXPECT_EQ(quote.status, QuoteStatus::RiskBucketMismatch);
  EXPECT_FALSE(quote.ok());
}

TEST(Quoting, UnpriceableContractsAreReported) {
  const VolSurface surface = FlatSurface();

  for (const ContractId contract :
       {ContractId{.time = 0.0, .strike = 4500.0, .type = OptionType::Call},
        ContractId{.time = 0.25, .strike = 0.0, .type = OptionType::Call}}) {
    EXPECT_EQ(make_quote(contract, surface, FlatRisk()).status, QuoteStatus::NotPriceable);
  }

  const VolSurface empty{};
  EXPECT_EQ(make_quote(AtTheMoney(), empty, FlatRisk()).status, QuoteStatus::NotPriceable);
}

// End to end: fit a surface from a generated chain, mark a real book against
// it, and quote a ladder that leans away from the risk the book is carrying.
TEST(Quoting, QuotesALadderAgainstASurfaceFittedFromAChain) {
  const VolSurface surface = fit_surface(generate_chain(ChainConfig{}));
  ASSERT_TRUE(surface.ok());

  PositionBook book;
  book.add(ContractId{.time = 0.25, .strike = 4500.0, .type = OptionType::Call}, 800.0);
  const PortfolioRisk risk = compute_risk(book, surface);
  ASSERT_GT(risk.vega, 0.0);

  const std::vector<ContractId> ladder = {
      {.time = 0.25, .strike = 4300.0, .type = OptionType::Put},
      {.time = 0.25, .strike = 4500.0, .type = OptionType::Call},
      {.time = 0.25, .strike = 4700.0, .type = OptionType::Call},
      {.time = 1.00, .strike = 4500.0, .type = OptionType::Call}};

  const std::vector<Quote> quotes = quote_ladder(ladder, surface, risk);
  ASSERT_EQ(quotes.size(), ladder.size());

  for (const Quote& quote : quotes) {
    ASSERT_TRUE(quote.ok()) << "K=" << quote.contract.strike;
    EXPECT_GE(quote.bid, 0.0);
    EXPECT_LT(quote.bid, quote.ask);
    EXPECT_GT(quote.theoretical_volatility, 0.0);
  }

  // The contract the book is long in gets skewed down; an untouched bucket at
  // a different expiry does not.
  const Quote& held = quotes[1];
  const Quote& other_expiry = quotes[3];
  EXPECT_LT(held.quoted_mid_volatility, held.theoretical_volatility);
  EXPECT_DOUBLE_EQ(other_expiry.quoted_mid_volatility, other_expiry.theoretical_volatility);
}

}  // namespace
}  // namespace skewdesk
