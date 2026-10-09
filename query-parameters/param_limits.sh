#!/usr/bin/env bash
# param_limits.sh -- measured companion to "query parameters: 65,535" in the
# PostgreSQL Limits appendix.
#
# It builds a throwaway server from the PostgreSQL build that PG_CONFIG points at,
# loads t(id bigint primary key, payload text) with ROWS rows, compiles a tiny libpq
# client against the same build, and then:
#
#   (A) cost curve: for every N in PARAM_STEPS, the same logical operation done two
#       ways, ROUNDS times each, alternating which goes first:
#         lookup  in_list       WHERE id IN ($1, ..., $N)          N parameters
#                 any_array     WHERE id = ANY($1::bigint[])       1 parameter
#         insert  insert_values VALUES ($1,$2), ... (N/2 rows)      N parameters
#                 insert_unnest SELECT * FROM unnest($1, $2)        2 parameters
#   (B) the hard wall: an IN-list with 65,535 and with 65,536 Bind parameters, sent
#       through libpq and again over the raw protocol (no libpq), with every outcome
#       captured verbatim; then = ANY($1::bigint[]) with WALL_ARRAY elements in one
#       parameter.
#   (C) the practical crossover: the smallest N from which the one-parameter form
#       is faster, read from the medians and their 95% confidence intervals.
#
# All statements use the extended query protocol (PQexecParams): the values are
# real Bind parameters, never literals in the SQL text.
#
# Linux or macOS. Needs: bash, awk, a C compiler, libpq headers, and initdb/pg_ctl/
# psql from the same build. python3 (standard library only) writes the summary.
#
# Environment (all optional):
#   PG_CONFIG      pg_config of the build to test           (default: pg_config in PATH)
#   ROWS           rows in t                                (default: 3000000)
#   ROUNDS         measured rounds per point, after warm-up (default: 15)
#   PARAM_STEPS    values of N                              (default: 1 10 100 1000 10000 32000 65000)
#   WALL_ARRAY     elements in the one-parameter array of (B) (default: 2000000)
#   WORKDIR        scratch directory, wiped at start        (default: ./param-limits-work)
#   PORT           server port (Unix socket only)           (default: 5611)
#   SHARED_BUFFERS                                          (default: 1GB)
#   CC             C compiler                               (default: cc)
#   KEEP           1 = keep WORKDIR's data directory        (default: 0)
#   ALLOW_ASSERTS  1 = run even on a build with assertions  (default: 0)
#   SOCKET_DIR     directory for the Unix socket            (default: a fresh one under /tmp)
set -euo pipefail

PG_CONFIG=${PG_CONFIG:-pg_config}
ROWS=${ROWS:-3000000}
ROUNDS=${ROUNDS:-15}
PARAM_STEPS=${PARAM_STEPS:-1 10 100 1000 10000 32000 65000}
WALL_ARRAY=${WALL_ARRAY:-2000000}
WORKDIR=${WORKDIR:-$PWD/param-limits-work}
PORT=${PORT:-5611}
SHARED_BUFFERS=${SHARED_BUFFERS:-1GB}
CC=${CC:-cc}
KEEP=${KEEP:-0}

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$("$PG_CONFIG" --bindir)
DATA=$WORKDIR/data
# Unix socket paths are limited to ~107 bytes, so the socket lives in a short
# temporary directory instead of under WORKDIR (which may be long).
SOCK=${SOCKET_DIR:-$(mktemp -d /tmp/pl.XXXXXX)}
CLIENT=$WORKDIR/paramclient
CONNINFO="host=$SOCK port=$PORT user=postgres dbname=postgres"
export PGHOST=$SOCK PGPORT=$PORT PGUSER=postgres PGDATABASE=postgres PGOPTIONS="-c client_min_messages=warning"

q() { "$BIN/psql" -X -q -At -v ON_ERROR_STOP=1 -c "$1"; }

cleanup() {
  "$BIN/pg_ctl" -D "$DATA" -m fast -w stop >/dev/null 2>&1 || true
  [ "$KEEP" = 1 ] || rm -rf "$DATA"
  [ -n "${SOCKET_DIR:-}" ] || rm -rf "$SOCK"
}
trap cleanup EXIT

if "$PG_CONFIG" --configure 2>/dev/null | grep -q -- '--enable-cassert'; then
  echo "This build has assertions enabled (--enable-cassert): timings would be meaningless." >&2
  [ "${ALLOW_ASSERTS:-0}" = 1 ] || { echo "Set ALLOW_ASSERTS=1 to run anyway." >&2; exit 2; }
fi

# ---------------------------------------------------------------- setup
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR" "$SOCK"

echo "== Compiling the libpq client against $("$PG_CONFIG" --version)"
"$CC" -O2 -Wall -I"$("$PG_CONFIG" --includedir)" "$HERE/paramclient.c" \
  -L"$("$PG_CONFIG" --libdir)" -Wl,-rpath,"$("$PG_CONFIG" --libdir)" -lpq -o "$CLIENT"

"$BIN/initdb" -D "$DATA" -U postgres -A trust -E UTF8 >/dev/null
cat >> "$DATA/postgresql.conf" <<CONF
port = $PORT
listen_addresses = ''
unix_socket_directories = '$SOCK'
shared_buffers = $SHARED_BUFFERS
autovacuum = off
# Commit flushes are not what is measured; they would only add disk noise to the inserts.
synchronous_commit = off
# Same plan for both approaches, compiled or not: keep JIT out of the comparison.
jit = off
CONF
"$BIN/pg_ctl" -D "$DATA" -l "$WORKDIR/server.log" -w start >/dev/null
if [ "$(q 'show debug_assertions')" = on ] && [ "${ALLOW_ASSERTS:-0}" != 1 ]; then
  echo "debug_assertions is on in the running server: timings would be meaningless." >&2
  exit 2
fi

echo "== Loading $ROWS rows into t(id bigint primary key, payload text)"
q "CREATE TABLE t (id bigint PRIMARY KEY, payload text)"
q "INSERT INTO t SELECT g, md5(g::text) || md5((g + 1)::text) FROM generate_series(1, $ROWS) g"
q "CREATE TABLE ins (id bigint PRIMARY KEY, payload text)"
q "VACUUM ANALYZE t"
if q "CREATE EXTENSION IF NOT EXISTS pg_prewarm" 2>/dev/null; then
  q "SELECT pg_prewarm('t'), pg_prewarm('t_pkey')" >/dev/null
fi
{
  echo "server_version $(q 'show server_version')"
  echo "rows $ROWS"
  echo "table_size $(q "SELECT pg_size_pretty(pg_total_relation_size('t'))")"
  echo "rounds $ROUNDS"
  echo "param_steps $PARAM_STEPS"
} > "$WORKDIR/environment.txt"

# ---------------------------------------------------------------- (B) the wall
echo "== (B) The hard wall, triggered"
{
  # through libpq, as an application would
  "$CLIENT" "$CONNINFO" wall 65535 "$ROWS"
  "$CLIENT" "$CONNINFO" wall 65536 "$ROWS"
  # raw protocol, no libpq: N goes into the Int16 count field of the Bind message
  "$CLIENT" "$SOCK" wire 65535 "$ROWS" "$PORT"
  "$CLIENT" "$SOCK" wire 65536 "$ROWS" "$PORT"
  # the same lookup with every key in ONE parameter
  "$CLIENT" "$CONNINFO" bigarray "$WALL_ARRAY" "$ROWS"
} | tee "$WORKDIR/wall.txt"

# ---------------------------------------------------------------- (A) the curve
echo "== (A) Cost curve: $ROUNDS rounds per point after one warm-up"
echo "kind,approach,n,bind_params,round,planning_ms,exec_ms,total_ms" > "$WORKDIR/raw.csv"
for n in $PARAM_STEPS; do
  if [ "$n" -gt 65535 ]; then
    echo "skipping N=$n: above the protocol limit (see the wall above)" >&2
    continue
  fi
  printf '   N=%-6s lookup' "$n"
  "$CLIENT" "$CONNINFO" lookup "$n" "$ROUNDS" "$ROWS" "$n" >> "$WORKDIR/raw.csv"
  if [ "$n" -ge 2 ]; then
    printf ' insert'
    "$CLIENT" "$CONNINFO" insert "$n" "$ROUNDS" >> "$WORKDIR/raw.csv"
  fi
  echo
done

# ---------------------------------------------------------------- (C) report
echo
python3 "$HERE/report.py" "$WORKDIR" | tee "$WORKDIR/summary.txt"
echo
echo "Files: $WORKDIR/{raw.csv,summary.csv,summary.txt,wall.txt,params.svg,environment.txt}"
