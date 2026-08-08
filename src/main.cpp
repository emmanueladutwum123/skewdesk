#include "skewdesk/black_scholes.hpp"

#include <cstdio>
#include <initializer_list>

// A small demonstration: price a symmetric strike ladder around a 4500-level
// index and print the greeks a desk would actually look at. The wing strikes
// are there to show delta saturating toward the dividend discount factor on
// one side and toward zero on the other, with gamma and vega concentrating
// near the money -- the shape that motivates everything built on top of this
// in later milestones.
int main() {
  constexpr double kSpot = 4500.0;

  std::printf("%-10s %-6s %10s %9s %10s %10s %11s\n", "strike", "type", "price",
              "delta", "gamma", "vega", "theta");
  std::printf("%s\n", "----------------------------------------------------------------------------");

  for (const double strike : {3800.0, 4200.0, 4500.0, 4800.0, 5200.0}) {
    const skewdesk::BlackScholesInputs in{.spot = kSpot,
                                          .strike = strike,
                                          .rate = 0.042,
                                          .dividend = 0.013,
                                          .volatility = 0.18,
                                          .time = 0.25};

    for (const auto type : {skewdesk::OptionType::Call, skewdesk::OptionType::Put}) {
      const skewdesk::Greeks g = skewdesk::greeks(in, type);
      std::printf("%-10.0f %-6s %10.4f %9.4f %10.6f %10.3f %11.3f\n", strike,
                  type == skewdesk::OptionType::Call ? "call" : "put",
                  skewdesk::price(in, type), g.delta, g.gamma, g.vega, g.theta);
    }
  }

  return 0;
}
