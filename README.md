# PostgreSQL practical limits, measured

[![CI](https://github.com/Manuelreyesbravo/postgres-practical-limits/actions/workflows/ci.yml/badge.svg)](https://github.com/Manuelreyesbravo/postgres-practical-limits/actions/workflows/ci.yml)

The PostgreSQL documentation has an appendix of
[hard limits](https://www.postgresql.org/docs/current/limits.html): 1,600 columns per
table, 65,535 query parameters, 32 TB per relation (with 8 kB blocks), and so on. Next to the table it says
one sentence about everything else:

> However, practical limits, such as performance limitations or available disk space
> may apply before absolute hard limits are reached.

That sentence is right, and it is where the useful questions start. Where do the
practical limits appear? How does the cost grow on the way to the hard number? What
happens at the wall, and what does the alternative cost?

Each chapter here is a small, self-contained kit that measures one limit:
- It builds a throwaway server from the PostgreSQL build you point it at.
- It measures the cost curve up to the limit, with medians and 95% confidence
  intervals.
- It triggers the hard wall and captures the error verbatim.
- It reads the practical crossover off the data.

The README of each chapter shows one run on one machine, with the complete output
committed next to it.

## Chapters

| chapter | hard limit (Appendix K) | status |
|---|---|---|
| [query-parameters](query-parameters/) | query parameters: 65,535 | available |
| columns-per-table | columns per table: 1,600 | planned |
| rows-per-table | rows per table: limited by the tuples that fit in 4,294,967,295 pages | planned |
| [standby-replay-ceiling](https://github.com/Manuelreyesbravo/standby-replay-ceiling) | (replication; no hard number) | available, separate repository |

`standby-replay-ceiling` is the replication chapter. It measures the single-process WAL
replay ceiling of a streaming standby against WAL generation on the primary. Appendix K
has no row for that one, which is rather the point of this series.

## Ground rules

- **The scripts measure; the READMEs report.** Every number and every error message in
  a chapter README comes from a run of its script, and that run's output is committed.
- **Medians with 95% CIs over repeated rounds.** Read the intervals. Where two of them
  overlap, the difference is not established.
- **Builds with `--enable-cassert` are refused** (checked with `pg_config` and
  `debug_assertions`), because their timings say nothing about production.
- **The numbers are specific.** They depend on the hardware, the PostgreSQL version, the
  client library, and the data shape. The mechanism is what carries over. Run the kit on
  your own machine.
- **None of this is a new finding.** People who work on PostgreSQL know these limits. The
  kits make them visible with numbers.

## CI

CI runs `bash -n`, `shellcheck` and a Python syntax check. For each chapter it then runs
a functional smoke against PostgreSQL 17 and 18 from PGDG, on a tiny load. The hard walls
are checked for real, because they do not depend on the hardware. The timings in the
chapter READMEs are from the hardware described there, not from CI.

## License

Apache-2.0. See [LICENSE](LICENSE).
