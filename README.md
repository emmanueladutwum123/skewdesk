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
| M3 | Seeded synthetic option-chain generator with skew and term structure | **Done** |
| M4 | SVI surface fit with butterfly and calendar arbitrage checks | **Done** |
| M5 | Position book, portfolio greeks, vega bucketed by tenor and strike | **Done** |
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

## M3

A seeded generator producing full chains — multiple expiries, each with its own
forward, discount factor, strike ladder, and two-sided markets on both sides of
every strike. Real chain data is not reliably licensable for CI, so everything
downstream is tested against deterministic synthetic input, the same approach
used elsewhere in this portfolio.

**The generator deliberately does not use SVI**, even though M4 fits SVI. If
the two shared a functional form, M4's tests would only prove that a model can
recover its own parameters — circular, and silent on whether the fit copes with
a surface shaped by something other than itself. Real chains are not generated
by SVI either. The generator instead uses an empirical form in log-moneyness
`k = ln(strike / forward)`:

```
sigma(k, T) = atm(T) + slope(T) * k + curvature * (hypot(k, w) - w)
```

with two stylized facts of index options built in: the slope is negative
(downside strikes carry higher volatility, because index options are priced by
people hedging crash risk rather than by symmetric speculation), and it decays
as `1/sqrt(T)`, so a three-week skew is steep and a two-year skew nearly flat.

**The wing term is a smoothed absolute value, and that detail is load-bearing.**
The obvious choice — a plain quadratic in `k` — makes total variance grow like
`k⁴`, which violates Lee's moment formula. The consequence is not subtle. At
two years and 2.2× the forward, it lifted implied volatility fast enough that
call prices began *rising* with strike:

```
K= 8325  vol=0.250  price=49.45
K= 9025  vol=0.274  price=48.00
K= 9775  vol=0.302  price=50.50   <- rising
K=10600  vol=0.334  price=56.72   <- rising
```

That is a call spread with negative cost and a non-negative payoff — free
money, and a chain M4 could never legitimately fit. `hypot(k, w) - w` behaves
like `k²/(2w)` near the money, so the smile there is unchanged, but grows
linearly in `|k|` in the wings, which keeps prices monotone and convex across
the entire generated band.

**Markets are quoted in volatility terms.** The half-spread is
`vega × half_spread_vol_points`, floored at a minimum tick, which automatically
produces narrow price-space markets in the wings and wider ones near the money
— the opposite of a fixed price spread, and what real chains actually look
like. Optional per-contract volatility noise perturbs each side independently,
so put-call parity stops holding exactly and the M2 regression's averaging and
R² diagnostic have something real to do.

### Testing

45 tests. Added here:

- **Static-arbitrage checks on every generated chain**: call prices strictly
  decreasing in strike, and convex against the general non-uniform-ladder
  chord condition. This is what caught the quadratic wing blow-up.
- **Full M2 + M3 pipeline**: generate a chain from a known surface, recover the
  forward and discount from the chain alone, invert the out-of-the-money side,
  and compare against the surface that produced it.
- Reproducibility from the seed, and that a different seed actually differs.
- The stylized facts as assertions: downward slope across strikes, skew
  flattening with maturity, at-the-money term structure mean-reverting.
- The wing term's crossover — quadratic near the money, linear far out.

## M4

Fitting Gatheral's raw SVI parameterization of total implied variance to each
expiry, then checking the assembled surface for both kinds of static
arbitrage.

```
w(k) = a + b * (rho * (k - m) + sqrt((k - m)^2 + sigma^2))
```

Total variance rather than volatility, because that is the quantity the
arbitrage conditions are naturally expressed in. Geometrically the function is
a hyperbola — two straight asymptotes with slopes `b(rho-1)` and `b(rho+1)`,
joined smoothly around a vertex near `k = m` — so every parameter maps onto
something a trader already thinks about.

**The fit is a two-dimensional search, not a five-dimensional one.** For any
fixed vertex position and width `(m, sigma)`, SVI is *linear* in its remaining
three parameters, so those fall out of an exact least-squares solve. That
collapse is what makes the problem tractable, and it is why the outer search is
a deterministic shrinking grid rather than a general-purpose optimizer: with a
cheap two-dimensional objective there is no pathological geometry to defend
against, and a grid gives byte-identical results across compilers.

**Butterfly arbitrage via Durrleman's condition.** A slice is free of butterfly
arbitrage exactly when

```
g(k) = (1 - k w'/(2w))^2 - (w'^2/4)(1/w + 1/4) + w''/2  >=  0
```

everywhere. What `g` really is: a quantity proportional to the risk-neutral
probability density the slice implies. A negative value is a negative
probability, which cashes out as a butterfly spread costing less than nothing.
Checking the density directly is strictly stronger than spot-checking
butterflies at the listed strikes, because it catches violations *between*
strikes that no tradeable butterfly would reveal.

**Calendar arbitrage in total variance, at fixed log-moneyness.** Total
variance must never decrease with maturity. Both qualifiers matter: implied
*volatility* routinely falls with maturity without any arbitrage — that is just
the term structure — so a check phrased in volatility would raise false alarms
on perfectly ordinary surfaces. There is a test asserting exactly that case.

**Violations are reported, not repaired.** The fitter pulls parameters into the
region where the slice is a valid variance curve — non-negative wing angle,
correlation strictly inside (−1, 1), total variance never negative — but it
deliberately does *not* force the butterfly condition. A violation is a fact
about the market data, and silently massaging it away would destroy the signal
a market maker most needs.

Interpolation across maturities is linear in total variance, which is the only
choice that cannot manufacture calendar arbitrage between two arbitrage-free
slices.

### Results

The full pipeline on a generated chain — parity recovery, inversion, SVI fit,
arbitrage checks (`./skewdesk_demo`):

```
     T   forward   pts        a        b     rho        m    sigma     rmse(w)      min g
  0.02   4502.61    21 -0.00314  0.01013  -0.749  -0.2109   0.4674    1.95e-08     0.1489
  0.08   4510.45    21 -0.00058  0.01073  -0.986  -0.1157   0.3265    5.12e-07     0.2554
  0.25   4532.74    21  0.00030  0.02321  -0.841  -0.0999   0.3415    1.14e-05     0.2886
  0.50   4565.73    21  0.00367  0.03991  -0.731  -0.0836   0.3256    3.28e-05     0.2985
  1.00   4632.41    21  0.00700  0.08198  -0.576  -0.0838   0.3538    1.36e-04     0.2742
  2.00   4768.72    21  0.00450  0.17694  -0.448  -0.0925   0.4073    5.53e-04     0.2241

Calendar check: worst total-variance gap = +1.245377e-03 at k = +0.064
Worst reproduction error vs the generating surface: 0.001286 absolute volatility
```

`rho` is negative at every expiry and its magnitude falls monotonically with
maturity (−0.75 → −0.45) — the fit recovering the skew flattening that the
generator put in, without ever being told the functional form. Durrleman's `g`
stays comfortably positive across every slice, and the worst calendar gap is
positive.

### Testing

66 tests. Added here:

- **Exact recovery**: fitted against observations sampled from a known SVI
  curve, the fit reproduces it to an RMSE below 1e-8 in total variance.
- **Analytic derivatives against finite differences**, the same discipline used
  on the M1 greeks — `w'` and `w''` feed Durrleman's condition, so an error
  there would silently disable the arbitrage check.
- **A confirmed butterfly-violating slice is detected**, and the fitter reports
  `ButterflyArbitrage` while still returning the parameters for inspection.
- **A falling-volatility term structure is not flagged** as calendar arbitrage,
  pinning the total-variance-versus-volatility distinction.
- **Interpolation never decreases total variance** across the whole maturity
  range.
- The end-to-end fit of a generated chain is required to come out free of both
  arbitrage types, and to reproduce the generating surface to within 2e-3
  absolute volatility.

## M5

A position book netted by contract, marked against the fitted surface, with
risk aggregated the way an options desk actually looks at it.

**The point of this milestone is that a single vega number is not risk
management.** A book long 1,000 vega in the front month and short 1,000 vega in
the two-year has net vega of zero and an enormous position — it is short the
term structure, and any differential move hurts. The same is true across
strikes: long downside vega against short upside vega is a skew position, not a
flat one. So vega is broken out on a tenor × log-moneyness grid, and reported
alongside a **gross** figure that sums the absolute value of each cell. The gap
between net and gross is exactly the offsetting exposure the aggregate hides.

On the demo book, net vega is **1.3% of gross**:

```
Book risk  --  5 positions priced, 0 skipped
  value -1677166   delta -6390.9   gamma +100.3226   theta/day -62890
  net vega -739371   term-weighted vega +33567893   GROSS vega 58304207

Vega by tenor x log-moneyness (net vega is 1.3% of gross)
                  k < -0.10 [-.10, -.03)  [-.03, .03)   [.03, .10)    k >= 0.10
       [0, 1M)            0            0     28782418            0            0   | +28782418
      [1M, 3M)            0            0            0            0            0   | +0
      [3M, 6M)     -8300626            0            0            0            0   | -8300626
      [6M, 1Y)            0            0            0            0            0   | +0
      [1Y, 2Y)            0            0    -10305967            0            0   | -10305967
     [2Y, inf)            0            0            0    -10915196            0   | -10915196
```

**Term-weighted vega.** A vega point at three weeks is not the same risk as one
at two years, because short-dated implied volatility moves far more. Vega is
therefore also reported scaled by `sqrt(reference / T)` against a three-month
reference, which puts the two on a comparable footing.

**Greeks are sticky-strike, and that is stated rather than buried.** They are
Black-Scholes greeks evaluated at the surface's volatility with the surface
held fixed, so delta excludes the way implied volatility at a given strike
tends to move when spot moves. The alternative depends on a view about how the
surface travels; encoding such a view silently inside a number labelled "delta"
would be worse than excluding it openly.

**Forwards and discount factors interpolate in log space**, because that is
where they are linear — `ln(forward)` grows at the cost-of-carry rate and
`ln(discount)` falls at the interest rate, so a straight line in log space is a
constant rate between nodes. Maturity zero is a known node for both (the
forward is the spot, the discount factor is one), so the short end is
interpolated rather than extrapolated.

The risk code reuses the M1 spot-parameterization greeks via an exact
conversion from market coordinates, rather than restating the formulas in
forward terms — the M1 versions are the ones validated against finite
differences, so restating them would mean re-earning that confidence.

### Testing

81 tests. Added here:

- **Vega against a finite difference of the book's own mark**, by shifting a
  flat surface's volatility — validating aggregation and scaling together, not
  just the per-contract formula.
- **Bucket sums reconcile**: by-tenor, by-moneyness, and the full grid each sum
  to net vega.
- **The offsetting-position case is asserted directly** — net vega round to
  zero while gross vega stays near twice the leg size, with the two legs
  landing in different tenor buckets with opposite signs.
- Log-space forward and discount interpolation reproduce a constant rate
  exactly, including back to maturity zero and beyond the final node.
- Book netting, including that a contract netted flat is removed rather than
  left as a zero row, and that calls and puts at one strike stay distinct.
- Malformed positions are skipped and counted rather than silently mispriced.

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
