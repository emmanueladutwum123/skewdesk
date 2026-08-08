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
| M2 | Implied-vol solver; forward and discount recovered via put-call parity | **Done** |
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

## M2

Reading a market rather than assuming one: recover the forward and discount
factor from the chain itself, then invert quoted prices into implied
volatilities.

**Put-call parity as a regression.** For an equity index neither the financing
rate nor the dividend stream is directly observable, but the option chain
prices them jointly. Parity says `call - put = discount * (forward - strike)`,
so regressing `call - put` on strike across one expiry gives a line whose slope
is `-discount` and whose intercept is `discount * forward`. Every strike is an
independent observation of the same two unknowns, so fitting across all of them
also averages away quote noise, and the R² of that fit is a free data-quality
signal — parity is an exact arbitrage relation, so anything materially below 1
means the quotes disagree with each other and the recovered forward should not
be trusted. A non-negative slope is rejected outright: it implies a
non-positive discount factor, which means the input was never a coherent
same-expiry chain.

**Safeguarded Newton for the inversion.** A Newton step is taken only when it
lands inside the current bracket and is at least halving the interval;
otherwise the solver bisects. That is what makes the far wings tractable — at
strike 7000 against a 4500 forward, vega is 1.5e-15 while the option is still
worth a perfectly ordinary 4e-18, so an unguarded Newton step divides by
approximately zero and diverges. The rejection conditions are written so that a
vanishing vega forces bisection on its own, with no special case for it.

**The conditioning limit, stated rather than hidden.** Deep in-the-money quotes
cannot be inverted at all. At forward 4500, strike 3000, and three weeks to
expiry, volatilities of 8%, 15% and 30% produce *byte-identical* prices: the
time value that carries the volatility information has fallen below the
resolution of a double sitting beside an intrinsic value of ~4500. The solver
reports `IllConditioned` instead of returning a number, and the documented
remedy is the one desks already use — invert the out-of-the-money side, whose
value is entirely time value, and recover the other through parity.

That distinction is why the conditioning test is *relative to intrinsic value*
rather than an absolute price tolerance. An absolute tolerance wide enough to
be useful near the money would swallow a far-wing option legitimately worth
1e-73 and report zero volatility for a contract whose implied volatility is
perfectly ordinary. Out of the money the intrinsic value is zero, so the same
threshold collapses to "is the quote strictly positive," which is exactly
right.

### Testing

32 tests. The ones added here:

- **Round-trip recovery** — price at a known volatility, invert, recover it to
  1e-9 — across 9 strikes × 4 tenors × 4 volatilities, inverting the
  out-of-the-money side at each strike.
- **End-to-end with no quoted rate**: build a chain from a known forward,
  discount, and downward-sloping skew; recover the forward and discount by
  regression; then back out the smile using only the recovered values. Getting
  the original skew back means every component agrees.
- **Call and put agree** where both sides are invertible — the invariant a real
  chain gets checked against.
- **The ill-conditioning limit is pinned as a test**, including an assertion
  that the two prices really are identical, so the limitation stays documented
  behaviour rather than something rediscovered later.
- No-arbitrage rejection: quotes below intrinsic, above the ceiling, and
  malformed contracts each return their own status rather than a wrong number.

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
