# skewdesk

A C++20 equity index options market-making engine: fit an arbitrage-free
volatility surface to an option chain, price and risk a book against it, and
generate two-sided quotes that lean away from the risk the book is already
carrying.

[![CI](https://github.com/emmanueladutwum123/skewdesk/actions/workflows/ci.yml/badge.svg)](https://github.com/emmanueladutwum123/skewdesk/actions/workflows/ci.yml)

## Why

A market maker in index options does not make money by predicting direction. It
quotes both sides continuously, earns the difference between where it transacts
and where its model says fair value is, and survives by managing the greek
exposure that accumulates as a by-product. That makes the fair-value model — a
volatility *surface*, not a single number — the actual competitive asset, and it
makes risk management the thing that converts theoretical edge into realized
P&L.

`skewdesk` is built to make both of those explicit rather than implied.

## Status

Built incrementally, one milestone at a time.

| Milestone | Scope | Status |
|---|---|---|
| M1 | Black-Scholes-Merton pricer and analytic greeks, CMake + 4-leg CI | **Done** |
| M2 | Implied-vol solver; forward and discount recovered via put-call parity | Planned |
| M3 | Seeded synthetic option-chain generator with skew and term structure | Planned |
| M4 | SVI surface fit with butterfly and calendar arbitrage checks | Planned |
| M5 | Position book, portfolio greeks, vega bucketed by tenor and strike | Planned |
| M6 | Quoting engine: theo, risk-scaled width, inventory skew | Planned |
| M7 | Simulation and P&L attribution; adverse-selection measurement | Planned |
| M8 | Benchmarks, `DESIGN.md`, `BENCHMARKS.md` | Planned |

## M1

A Black-Scholes-Merton pricer for European options on an asset paying a
continuous dividend yield, with analytic delta, gamma, vega, theta, and rho.

Three decisions worth naming:

**Pricing is expressed on a forward basis internally.** The forward and spot
formulations are algebraically identical, but only the forward one survives
contact with real chain data — for index options the effective forward and
discount factor are *recovered from the market* through put-call parity rather
than assumed from a quoted rate and dividend yield. Structuring it this way now
means M2 can substitute a parity-implied forward without touching the pricing
core.

**The normal CDF uses `erfc`, not `erf`.** The familiar
`0.5 * (1 + erf(x / sqrt(2)))` catastrophically cancels for `x` well below zero:
it adds 1 to something within an ulp of −1 and loses most of its significant
digits. `0.5 * erfc(-x / sqrt(2))` stays accurate far into the left tail, which
is precisely where deep out-of-the-money options live — and those are a large
share of any index chain.

**Degenerate inputs are limit cases, not errors.** Zero time to expiry, zero
volatility, zero spot, or zero strike all make `d1` and `d2` undefined rather
than merely large, but each has an unambiguous limiting value. The pricer
returns discounted intrinsic value on the forward; `greeks` returns the
dividend-discounted step function for delta and zero elsewhere, which is exact
for gamma and vega and a documented convention for theta and rho at the kink.

### Testing

14 tests. The ones that carry weight:

- **Greeks validated against central finite differences** across a 90-case grid
  of strikes, volatilities, tenors, and dividend yields, for both calls and
  puts. Every analytic formula is checked against a numerical derivative of the
  pricer rather than against another closed form, so a transcription error in
  any greek fails immediately.
- **Put-call parity** across the same grid — an invariant that references no
  model internals at all, so a sign error or a misplaced discount factor in
  either branch breaks it.
- **Theta's sign convention.** Theta is the derivative with respect to calendar
  time, and time-to-expiry shrinks as calendar time advances. The finite-
  difference check negates accordingly; an implementation that gets this
  backwards passes every other test in the suite.
- **Left-tail accuracy of the normal CDF**, pinned against a reference value at
  −10 standard deviations, where the naive `erf` formulation returns a flat
  zero.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure
./skewdesk_demo
```

Sanitizer build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSD_ENABLE_ASAN=ON -DSD_ENABLE_UBSAN=ON
```

CI runs four legs: gcc and clang, each in Debug with ASan/UBSan and in Release,
all with `-Werror`.

## License

MIT — see [LICENSE](LICENSE).
