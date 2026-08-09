# Benchmarks

Every number below was captured by running the benchmarks directly, not
estimated. Reproduce with:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_BENCHMARKS=ON
cmake --build build-bench
./build-bench/bench/bench_pricing
```

## Measurement environment

Apple M2, 8 cores, macOS 26.2, Apple clang 15, `-O2`, Release.

Two honest caveats. Google Benchmark reports `Unable to determine clock rate
from sysctl: hw.cpufrequency` and `Failed to set thread affinity` on Apple
Silicon — neither affects the wall-clock measurements, but it does mean the
usual CPU-frequency metadata is absent. And these are single-machine numbers on
a laptop with an asymmetric core layout; treat the *ratios* between rows as the
durable result and the absolute nanoseconds as indicative.

CI compile-checks the benchmarks on the Release legs with `-Werror` but never
runs them. A shared runner produces noisy, non-representative timings, and a
number that cannot be trusted is worse than no number.

## Pricing

| Benchmark | Time |
|---|---:|
| `BM_NormalCdf` | 9.80 ns |
| `BM_Price` | 18.6 ns |
| `BM_PriceDegenerate` | 3.68 ns |
| `BM_ForwardVega` | 10.5 ns |
| `BM_Greeks` | 36.3 ns |

A valuation costs 18.6 ns, of which two normal-CDF evaluations account for
roughly 19.6 ns of transcendental work — the degenerate branch, which skips the
logarithm and both error functions entirely, runs in 3.68 ns, so the
transcendentals are about **5x the cost of the surrounding arithmetic**.

All five sensitivities cost 36.3 ns, under twice a bare price, because they
share one set of moments: `d1`, `d2` and the normal density are computed once
and reused across delta, gamma, vega, theta and rho.

## Implied volatility

| Benchmark | Time | vs at-the-money |
|---|---:|---:|
| `BM_ImpliedVolAtTheMoney` | 167 ns | 1.0x |
| `BM_ImpliedVolDeepWing` | 1637 ns | **9.8x** |
| `BM_ImpliedVolIllConditioned` | 3.93 ns | 0.02x |

**This is the benchmark that justifies the solver's design.** Near the money
vega is large, Newton converges quadratically, and the whole inversion costs
about nine price evaluations. At strike 7000 against a 4500 forward, vega has
collapsed to roughly 1.5e-15 while the option is still worth an ordinary
4e-18 — an unguarded Newton step divides by approximately zero and diverges, so
the safeguard conditions force bisection instead. Bisection converges linearly
and needs about forty iterations to close the bracket to tolerance, which is
exactly the 9.8x observed.

The cost is real and it is worth paying: the alternative is not a faster answer
but no answer at all in the wings, which is where a volatility surface most
needs points.

The third row is the ill-conditioned deep in-the-money case. At 3.93 ns it is
faster than a single valuation, because the quote is rejected on its
conditioning before any iteration begins. Refusing to answer is the cheapest
path through the solver, as it should be.

## Surface

| Benchmark | Time |
|---|---:|
| `BM_ParityFit` | 49.8 ns |
| `BM_SviSliceFit` | 313 µs |
| `BM_SviSliceFitCoarse` | 61.3 µs |
| `BM_DurrlemanCheck` | 2.51 µs |
| `BM_GenerateChain` | 17.5 µs |
| `BM_FitFullSurface` | 1.99 ms |
| `BM_SurfaceLookup` | 9.88 ns |

Recovering the forward and discount factor from put-call parity costs 49.8 ns
for a whole expiry — a single pass of centred accumulations. There is never a
performance reason to assume a rate instead of reading it from the chain.

The SVI slice fit dominates everything else at 313 µs, and the reason is
visible in the coarse variant: dropping the search from 21 grid points and 5
refinements to 11 and 3 cuts it to 61.3 µs, a **5.1x** reduction. The cost is
the outer search's grid, not the inner algebra — each grid point is an exact
least-squares solve over 21 observations, which is cheap; there are simply
`21 × 21 × 6` of them.

A full surface — parity recovery, out-of-the-money inversion, and an SVI fit
for each of six expiries, plus both arbitrage checks — takes 1.99 ms, about
332 µs per expiry, consistent with the slice fit plus its inversions.

Evaluating a fitted surface costs 9.88 ns, four orders of magnitude below
fitting it. Fitting is a periodic recalibration; lookup is the hot path.

## Portfolio risk

| Benchmark | Positions | Time | Throughput |
|---|---:|---:|---:|
| `BM_ComputeRisk` | 10 | 3.23 µs | 3.12 M/s |
| `BM_ComputeRisk` | 100 | 28.4 µs | 3.53 M/s |
| `BM_ComputeRisk` | 1,000 | 287 µs | 3.52 M/s |
| `BM_ComputeRisk` | 4,000 | 1.14 ms | 3.51 M/s |
| `BM_BuildBook` | 100 | 3.13 µs | |
| `BM_BuildBook` | 1,000 | 253 µs | |
| `BM_BuildBook` | 4,000 | 3.17 ms | |

Marking and bucketing a book is **linear**: throughput holds within 1% from 100
to 4,000 positions. At 3.5M positions per second, recomputing risk before every
quote refresh costs less than quoting does, which is why the quoting engine
takes risk as an input rather than caching it.

`BM_BuildBook` is visibly **quadratic** — ten times the positions costs eighty
times the work — because `PositionBook::add` nets by linear scan. That is a
deliberate trade: real books are hundreds of contracts, where the scan's
constant beats a map's per-lookup overhead, and the quadratic term only becomes
visible at sizes an options book does not reach.

### A benchmark that was measuring itself

The first version of `BM_ComputeRisk` reported **17.6 µs flat** at 100, 1,000
and 5,000 positions. That looked like an impressively cache-friendly
implementation and was nothing of the kind: the contract generator stepped
strikes on `i % 60` while choosing the expiry on `i % 6`, and since 60 is a
multiple of 6 the pair repeated after 60 contracts. Every later position netted
against an existing one, the book saturated at 60 entries, and the benchmark
was faithfully measuring its own generator's period.

The fix was to advance strike, expiry and option type on independent cycles,
and to report throughput from the book's *actual* size rather than the
requested one — so a generator that quietly stops producing distinct work shows
up in the number instead of hiding behind it.

## Quoting

| Benchmark | Time | Per contract |
|---|---:|---:|
| `BM_MakeQuote` | 279 ns | 279 ns |
| `BM_QuoteLadder` (20) | 6.53 µs | 327 ns |
| `BM_QuoteLadder` (100) | 31.2 µs | 312 ns |
| `BM_QuoteLadder` (500) | 155 µs | 310 ns |

A single two-sided market costs 279 ns: a surface lookup, a bucket lookup,
three width terms, and three valuations — theo plus both sides. Ladder quoting
holds a flat 310 ns per contract at 500 contracts, so the cost is genuinely per
quote with no super-linear term.

## Simulation

| Benchmark | Steps | Time | Per step |
|---|---:|---:|---:|
| `BM_SimulationStep` | 50 | 45.0 ms | 0.90 ms |
| `BM_SimulationStep` | 250 | 155 ms | 0.62 ms |
| `BM_SimulationStepCoarseFit` | 250 | 63.3 ms | 0.25 ms |

A full step — rebuild the surface, mark the book, quote a ladder, match flow,
hedge, advance the market, attribute the P&L, mark out matured trades — costs
0.62 ms at 250 steps.

Surface rebuilding dominates it, and the coarse-fit row proves it: relaxing
only the SVI search resolution gives a **2.4x** speedup on the whole loop. That
is the lever to pull for longer runs, and it comes at a cost in fit accuracy
that M4's reproduction test measures directly.

The per-step figure falls from 0.90 ms at 50 steps to 0.62 ms at 250. That is
not the loop getting faster with age — it is the fixed startup cost of the
first surface build amortising over more steps.
