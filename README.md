# Stats Framework

A framework that collects performance statistics of procedures and stores them
in a repository (e.g. a DB), built with a spec-first workflow:
design document → Quint spec (verified) → C++ implementation → model-based
testing (MBT) of the implementation against the spec.

## Layout
| Path | Content |
|---|---|
| `docs/statsDESIGN.md` | design document: the single source for the spec, the code and the tests |
| `docs/conclusions.md` | lessons learned, decisions, and prompts for generating each artifact |
| `docs/quintCommands.md` | Quint commands with explanations and measured run times |
| `spec/stats.qnt` | Quint spec (`StatsFramework`) and verification module (`StatsFrameworkVerification`) |
| `traces/` | ITF traces generated from the spec = MBT test cases (committed) |
| `src/` | C++17 implementation (`.h` and `.cpp`) |
| `tests/` | unit tests, MBT harness, in-memory fake repository, test access |
| `scripts/` | `check_all.sh`, `gen_traces.sh`, `common.sh` (shared settings, witness list) |

## Requirements
Quint ≥ 0.31 (`npm install -g @informalsystems/quint`), Java ≥ 21 (for
`quint verify`), `jq`, CMake ≥ 3.20, a C++17 compiler. GoogleTest and
nlohmann/json are downloaded by CMake.

## Run all checks
```bash
./scripts/check_all.sh          # everything (~2.5 min)
./scripts/check_all.sh --quick  # without full verification (~30 s)
```
It runs, stopping at the first failure:
1. Quint typecheck;
2. Quint scenario tests;
3. simulation: invariants, and every witness reached (coverage printed);
4. full verification: TLC (`stepTlc`) and Apalache (`stepApalache`);
5. traces up to date: regenerated into a temp directory and compared with
   `traces/`;
6. C++: build, unit tests, MBT on all traces.

## Workflow after a change
| Changed | Do |
|---|---|
| design document (behavior) | update `spec/stats.qnt` and `src/` accordingly, then as below |
| `spec/stats.qnt` | `./scripts/gen_traces.sh`, commit `traces/`, then `./scripts/check_all.sh` |
| `src/` or `tests/` | `./scripts/check_all.sh --quick` (or build and run the tests, below) |
| a new witness | add it to the spec and to `WITNESSES` in `scripts/common.sh` |

## Build and run the C++ tests only
```bash
cmake -S . -B build && cmake --build build -j
./build/stats_unit_tests     # unit tests
./build/stats_mbt_tests      # MBT: replays every trace in traces/
```

## Not implemented
- A production repository adapter (only the `IStatsRepository` interface and
  the in-memory fake used by the tests).
- Phase 2 (multi-threading); see "Phase 2" in the design document.
