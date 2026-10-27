# Design — real-time conversion

Working design documents for converting this fork to a real-time game with controller support and a modern renderer, keeping the simulation systems largely intact.

Separate from `doc/`, which is upstream's documentation.

## Documents

| # | Document | Status |
| --- | --- | --- |
| 01 | [Architecture as-is](01-architecture-as-is.md) — how the game works today, measured from source | Written |
| 02 | Target architecture — real-time tick, intent queue, sim/frontend split | **Unblocked — not started** |
| 03 | [Decision log](03-decision-log.md) — the five design calls, with options and tradeoffs | **Written, all 5 decided** |
| 04 | Cut list — what goes, what stays, what is deferred | Not started |
| 05 | Migration plan — stages and their exit criteria | **Unblocked — not started** |

Docs 02 and 05 were deliberately blocked on 03, since four of the five decisions change what the target architecture *is*. All five are now settled, so both are ready to write.

## Decisions

| # | Decision | Status | Outcome / recommendation |
| --- | --- | --- | --- |
| D1 | Move-point banking | **Decided 2026-10-26** | **Cap at `speed * 1.5`, uniform across all creatures** |
| D2 | Multi-action ticks | **Decided 2026-10-26** | **Drain for the spike; move-ordered scheduler as declared target** |
| D3 | Pause semantics | **Decided 2026-10-27** | **Hard pause on modal UI, plus auto-pause on `distraction_type`** |
| D4 | Activity pacing | **Decided 2026-10-27** | **Time-scale multiplier on the pacer; content never rebalanced** |
| D5 | Tick rate | **Decided 2026-10-26** | **250 ms per tick — 4 ticks/sec** |

All five settled in dependency order: D5 first, since it converts every move cost into a wall-clock duration and is the unit the others are argued in; then D1, which depends on the rate; then D2, which depends on D1's cap; then D3, and finally D4, which reuses D3's trigger vocabulary.

### Carried forward into docs 02 and 05

- **Two known-incorrect intermediate states are deliberate**, not oversights: D2's drain-until-empty loop, and D3's reliance on synchronous blocking reads to freeze the pacer. Both are bounded, both are recorded, and both have a named successor stage.
- **The move-ordered scheduler (D2) is the largest single stage.** Doc 03 records the tick-ordering invariants it must preserve.
- **Profile the 38 `neither` activities before raising D4's multiplier ceiling.** The `ACT_MULTIPLE_*` family re-scans the map per tick; the ceiling should be set by measurement, not chosen.
- **Render must be decoupled from tick with interpolation (D5).** Without it the game looks like it runs at 4 FPS.

**D2 is knowingly a two-stage decision:** the first playable keeps the existing drain-until-empty loop, which is incorrect but bounded by D1's cap. The move-ordered scheduler is the declared target and carries its own migration stage; doc 03 records the tick-ordering invariants it must preserve.

D3 turns out to be a prerequisite for the real-time spike rather than a follow-up — and modal pause turns out to be **free** for the first playable, since every verb-layer prompt is a synchronous blocking read on the main thread and therefore stalls the pacer by construction. The work is a visible paused indicator, not a pause mechanism. It becomes explicit pacer state only once verbs are resumable. See its entry.

### Consequences of D5 now fixed

At 4 ticks/sec, with movement on normal floor costing exactly 100 moves per orthogonal step and baseline speed 100:

- Movement is **4 tiles/sec**; `mon_wolf` at speed 165 covers 6.6 tiles/sec.
- A 24-hour game day is **6 hours** of wall clock.
- Rendering must be decoupled from the tick and interpolate between tick positions, or the game looks like it runs at 4 FPS.
- The 145 game-time `once_every()` throttles keep their cadence but fire 4x more often in real time.

## Conventions

- **Measured, not remembered.** Claims about the current codebase cite `file:line`. Doc 01 carries the commands used, so figures can be re-verified as the tree moves.
- **Describe reality separately from intent.** Doc 01 is strictly as-is. Anything aspirational belongs in 02 onward.
- **Decisions get recorded with their reasoning**, in doc 03, including options rejected.

## Baseline

Written against commit `b9a8c7aa2c` (2026-10-26). `src/` is ~771k lines across ~1,148 files; `data/` holds 6,566 JSON files.
