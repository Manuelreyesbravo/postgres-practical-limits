/*
 * paramclient.c -- minimal libpq client used by param_limits.sh.
 *
 * Every statement is sent with PQexecParams, i.e. through the extended query
 * protocol (Parse / Bind / Execute), so the values travel as real Bind
 * parameters and never as literals in the SQL text.
 *
 * Usage:
 *   paramclient CONNINFO lookup   N ROUNDS MAXID SEED
 *   paramclient CONNINFO insert   N ROUNDS
 *   paramclient CONNINFO wall     N MAXID
 *   paramclient CONNINFO bigarray N MAXID
 *   paramclient SOCKDIR  wire     N MAXID PORT
 *
 * wire skips libpq: it speaks protocol 3.0 directly over the Unix socket (trust
 * authentication only) so the server's own answer to the Bind message is seen.
 *
 * lookup / insert print one CSV line per approach and measured round:
 *   kind,approach,n,bind_params,round,planning_ms,exec_ms,total_ms
 * total_ms is the client-side wall time of the statement itself (send, parse,
 * plan, execute, receive). planning_ms and exec_ms come from a second run of the
 * same statement under EXPLAIN (ANALYZE, TIMING OFF) with the same parameters.
 * Round 0 is a warm-up and is not printed. The two approaches alternate which
 * one goes first in each round and always see the same keys.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libpq-fe.h>

typedef struct
{
	char	   *buf;
	size_t		len;
	size_t		cap;
} strbuf;

static void
sb_add(strbuf *s, const char *fmt,...)
{
	for (;;)
	{
		va_list		ap;
		int			n;

		if (s->cap - s->len < 64)
		{
			s->cap = s->cap ? s->cap * 2 : 4096;
			s->buf = realloc(s->buf, s->cap);
			if (!s->buf)
				exit(1);
		}
		va_start(ap, fmt);
		n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
		va_end(ap);
		if ((size_t) n < s->cap - s->len)
		{
			s->len += n;
			return;
		}
		s->cap *= 2;
		s->buf = realloc(s->buf, s->cap);
		if (!s->buf)
			exit(1);
	}
}

static void
sb_raw(strbuf *s, const void *data, size_t len)
{
	while (s->cap - s->len < len + 1)
	{
		s->cap = s->cap ? s->cap * 2 : 4096;
		s->buf = realloc(s->buf, s->cap);
		if (!s->buf)
			exit(1);
	}
	memcpy(s->buf + s->len, data, len);
	s->len += len;
}

static void
sb_reset(strbuf *s)
{
	s->len = 0;
	if (s->buf)
		s->buf[0] = '\0';
}

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint64_t rng_state;

static long long
next_key(long long maxid)
{
	/* xorshift64: cheap, deterministic for a given seed */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (long long) (rng_state % (uint64_t) maxid) + 1;
}

static void
check(PGconn *conn, PGresult *res, const char *what)
{
	ExecStatusType st = PQresultStatus(res);

	if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
	{
		fprintf(stderr, "%s failed: %s", what, PQerrorMessage(conn));
		exit(1);
	}
}

static void
simple(PGconn *conn, const char *sql)
{
	PGresult   *res = PQexec(conn, sql);

	check(conn, res, sql);
	PQclear(res);
}

/* One statement, run twice: once timed by the client, once under EXPLAIN ANALYZE. */
static void
measure(PGconn *conn, const char *sql, int nparams, const char *const *values,
		const char *reset_sql, double *planning, double *exec, double *total)
{
	strbuf		ex = {0};
	PGresult   *res;
	double		t0;

	if (reset_sql)
		simple(conn, reset_sql);
	t0 = now_ms();
	res = PQexecParams(conn, sql, nparams, NULL, values, NULL, NULL, 0);
	*total = now_ms() - t0;
	check(conn, res, "statement");
	PQclear(res);

	if (reset_sql)
		simple(conn, reset_sql);
	sb_add(&ex, "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) %s", sql);
	res = PQexecParams(conn, ex.buf, nparams, NULL, values, NULL, NULL, 0);
	check(conn, res, "EXPLAIN");
	*planning = *exec = -1;
	for (int i = 0; i < PQntuples(res); i++)
	{
		const char *line = PQgetvalue(res, i, 0);
		const char *p;

		if ((p = strstr(line, "Planning Time: ")))
			*planning = atof(p + 15);
		else if ((p = strstr(line, "Execution Time: ")))
			*exec = atof(p + 16);
	}
	PQclear(res);
	free(ex.buf);
}

/* SELECT ... WHERE id IN ($1, ..., $n): one Bind parameter per key */
static char *
in_list_sql(const char *select, int n)
{
	strbuf		s = {0};

	sb_add(&s, "%s WHERE id IN (", select);
	for (int i = 1; i <= n; i++)
		sb_add(&s, i == 1 ? "$%d" : ",$%d", i);
	sb_add(&s, ")");
	return s.buf;
}

static void
run_lookup(PGconn *conn, int n, int rounds, long long maxid, uint64_t seed)
{
	const char *select = "SELECT id, payload FROM t";
	char	   *sql_in = in_list_sql(select, n);
	const char *sql_any = "SELECT id, payload FROM t WHERE id = ANY($1::bigint[])";
	char	  **keys = malloc(sizeof(char *) * n);
	char	   *keybuf = malloc((size_t) n * 21);
	strbuf		arr = {0};

	for (int r = 0; r <= rounds; r++)
	{
		const char *one[1];

		rng_state = seed + (uint64_t) r * 7919 + 1;
		sb_reset(&arr);
		sb_add(&arr, "{");
		for (int i = 0; i < n; i++)
		{
			keys[i] = keybuf + (size_t) i * 21;
			snprintf(keys[i], 21, "%lld", next_key(maxid));
			sb_add(&arr, i ? ",%s" : "%s", keys[i]);
		}
		sb_add(&arr, "}");
		one[0] = arr.buf;

		for (int k = 0; k < 2; k++)
		{
			int			a = k ^ (r & 1);	/* alternate who goes first */
			double		pl, ex, tot;

			if (a == 0)
				measure(conn, sql_in, n, (const char *const *) keys, NULL, &pl, &ex, &tot);
			else
				measure(conn, sql_any, 1, one, NULL, &pl, &ex, &tot);
			if (r > 0)
				printf("lookup,%s,%d,%d,%d,%.3f,%.3f,%.3f\n",
					   a == 0 ? "in_list" : "any_array", n, a == 0 ? n : 1,
					   r, pl, ex, tot);
		}
		fflush(stdout);
	}
}

static void
run_insert(PGconn *conn, int n, int rounds)
{
	int			rows = n / 2;	/* two columns, so two parameters per row */
	strbuf		vs = {0};
	strbuf		ids = {0};
	strbuf		pays = {0};
	const char *sql_unnest =
		"INSERT INTO ins (id, payload) SELECT * FROM unnest($1::bigint[], $2::text[])";
	const char *reset = "TRUNCATE ins";
	char	  **vals = malloc(sizeof(char *) * rows * 2);
	char	   *valbuf = malloc((size_t) rows * 2 * 32);
	const char *two[2];

	sb_add(&vs, "INSERT INTO ins (id, payload) VALUES ");
	sb_add(&ids, "{");
	sb_add(&pays, "{");
	for (int i = 0; i < rows; i++)
	{
		sb_add(&vs, i ? ",($%d,$%d)" : "($%d,$%d)", 2 * i + 1, 2 * i + 2);
		vals[2 * i] = valbuf + (size_t) (2 * i) * 32;
		vals[2 * i + 1] = valbuf + (size_t) (2 * i + 1) * 32;
		snprintf(vals[2 * i], 32, "%d", i + 1);
		snprintf(vals[2 * i + 1], 32, "payload-%d", i + 1);
		sb_add(&ids, i ? ",%s" : "%s", vals[2 * i]);
		sb_add(&pays, i ? ",%s" : "%s", vals[2 * i + 1]);
	}
	sb_add(&ids, "}");
	sb_add(&pays, "}");
	two[0] = ids.buf;
	two[1] = pays.buf;

	for (int r = 0; r <= rounds; r++)
	{
		for (int k = 0; k < 2; k++)
		{
			int			a = k ^ (r & 1);
			double		pl, ex, tot;

			if (a == 0)
				measure(conn, vs.buf, rows * 2, (const char *const *) vals, reset, &pl, &ex, &tot);
			else
				measure(conn, sql_unnest, 2, two, reset, &pl, &ex, &tot);
			if (r > 0)
				printf("insert,%s,%d,%d,%d,%.3f,%.3f,%.3f\n",
					   a == 0 ? "insert_values" : "insert_unnest", n,
					   a == 0 ? rows * 2 : 2, r, pl, ex, tot);
		}
		fflush(stdout);
	}
}

/* One IN-list with exactly n Bind parameters; reports the outcome verbatim. */
static void
run_wall(PGconn *conn, int n, long long maxid)
{
	char	   *sql = in_list_sql("SELECT count(*) FROM t", n);
	char	  **keys = malloc(sizeof(char *) * n);
	char	   *keybuf = malloc((size_t) n * 21);
	PGresult   *res;
	double		t0, ms;

	rng_state = 42;
	for (int i = 0; i < n; i++)
	{
		keys[i] = keybuf + (size_t) i * 21;
		snprintf(keys[i], 21, "%lld", next_key(maxid));
	}
	t0 = now_ms();
	res = PQexecParams(conn, sql, n, NULL, (const char *const *) keys, NULL, NULL, 0);
	ms = now_ms() - t0;
	if (PQresultStatus(res) == PGRES_TUPLES_OK)
		printf("wall n_params=%d status=ok matched=%s ms=%.1f\n", n, PQgetvalue(res, 0, 0), ms);
	else
	{
		char	   *msg = PQerrorMessage(conn);
		size_t		len = strlen(msg);

		while (len && (msg[len - 1] == '\n' || msg[len - 1] == ' '))
			msg[--len] = '\0';
		printf("wall n_params=%d status=error message=\"%s\"\n", n, msg);
	}
	PQclear(res);
}

/* = ANY($1::bigint[]) with n elements in ONE Bind parameter */
static void
run_bigarray(PGconn *conn, int n, long long maxid)
{
	strbuf		arr = {0};
	const char *one[1];
	PGresult   *res;
	double		t0, ms;

	rng_state = 4242;
	sb_add(&arr, "{");
	for (int i = 0; i < n; i++)
		sb_add(&arr, i ? ",%lld" : "%lld", next_key(maxid));
	sb_add(&arr, "}");
	one[0] = arr.buf;
	t0 = now_ms();
	res = PQexecParams(conn, "SELECT count(*) FROM t WHERE id = ANY($1::bigint[])",
					   1, NULL, one, NULL, NULL, 0);
	ms = now_ms() - t0;
	check(conn, res, "big array");
	printf("bigarray n_params=1 elements=%d param_bytes=%zu status=ok matched=%s ms=%.1f\n",
		   n, arr.len, PQgetvalue(res, 0, 0), ms);
	PQclear(res);
}

/* ---- raw protocol 3.0, just enough for one Parse/Bind/Execute/Sync ---- */

static void
put16(strbuf *s, uint16_t v)
{
	unsigned char b[2] = {v >> 8, v & 0xff};

	sb_raw(s, b, 2);
}

static void
put32(strbuf *s, uint32_t v)
{
	unsigned char b[4] = {v >> 24, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff};

	sb_raw(s, b, 4);
}

/* Append one message: type byte, Int32 length (including itself), body. */
static void
put_msg(strbuf *out, char type, strbuf *body)
{
	sb_raw(out, &type, 1);
	put32(out, (uint32_t) body->len + 4);
	sb_raw(out, body->buf, body->len);
	sb_reset(body);
}

static void
read_full(int fd, void *buf, size_t len)
{
	size_t		got = 0;

	while (got < len)
	{
		ssize_t		r = read(fd, (char *) buf + got, len - got);

		if (r <= 0)
		{
			fprintf(stderr, "wire: connection closed\n");
			exit(1);
		}
		got += r;
	}
}

static void
write_full(int fd, const void *buf, size_t len)
{
	size_t		put = 0;

	while (put < len)
	{
		ssize_t		r = write(fd, (const char *) buf + put, len - put);

		if (r <= 0)
		{
			fprintf(stderr, "wire: write failed\n");
			exit(1);
		}
		put += r;
	}
}

/* Read one message; returns its type, body in *body (caller frees). */
static char
read_msg(int fd, char **body, uint32_t *blen)
{
	char		type;
	unsigned char l[4];
	uint32_t	len;

	read_full(fd, &type, 1);
	read_full(fd, l, 4);
	len = ((uint32_t) l[0] << 24 | (uint32_t) l[1] << 16 | (uint32_t) l[2] << 8 | l[3]) - 4;
	*body = malloc(len + 1);
	read_full(fd, *body, len);
	(*body)[len] = '\0';
	*blen = len;
	return type;
}

static void
run_wire(const char *sockdir, int n, long long maxid, int port)
{
	struct sockaddr_un addr = {0};
	strbuf		out = {0};
	strbuf		body = {0};
	char	   *sql = in_list_sql("SELECT count(*) FROM t", n);
	/* the Bind parameter count is an Int16 on the wire: only the low 16 bits fit */
	uint16_t	count = (uint16_t) n;
	char	   *msg;
	uint32_t	mlen;
	char		type;
	char		matched[32] = "";
	const char *sqlstate = "", *message = "";
	char	   *errbody = NULL;
	int			fd;

	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.s.PGSQL.%d", sockdir, port);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
		fprintf(stderr, "wire: cannot connect to %s\n", addr.sun_path);
		exit(1);
	}

	/* StartupMessage: no type byte */
	put32(&body, 196608);		/* protocol 3.0 */
	sb_raw(&body, "user\0postgres\0database\0postgres\0", 32);
	sb_raw(&body, "\0", 1);
	put32(&out, (uint32_t) body.len + 4);
	sb_raw(&out, body.buf, body.len);
	sb_reset(&body);
	write_full(fd, out.buf, out.len);
	sb_reset(&out);
	while ((type = read_msg(fd, &msg, &mlen)) != 'Z')
	{
		if ((type == 'R' && mlen >= 4 && memcmp(msg, "\0\0\0\0", 4) != 0) || type == 'E')
		{
			fprintf(stderr, "wire: needs trust authentication on the Unix socket\n");
			exit(1);
		}
		free(msg);
	}
	free(msg);

	/* Parse: unnamed statement, no parameter types given (the server infers N) */
	sb_raw(&body, "", 1);
	sb_raw(&body, sql, strlen(sql) + 1);
	put16(&body, 0);
	put_msg(&out, 'P', &body);

	/* Bind: the count field holds n mod 65536, followed by that many values */
	rng_state = 42;
	sb_raw(&body, "", 1);		/* portal */
	sb_raw(&body, "", 1);		/* statement */
	put16(&body, 0);			/* all parameters in text format */
	put16(&body, count);
	for (int i = 0; i < count; i++)
	{
		char		key[21];
		int			kl = snprintf(key, sizeof(key), "%lld", next_key(maxid));

		put32(&body, (uint32_t) kl);
		sb_raw(&body, key, kl);
	}
	put16(&body, 0);			/* all result columns in text format */
	put_msg(&out, 'B', &body);

	sb_raw(&body, "", 1);		/* Execute the unnamed portal, no row limit */
	put32(&body, 0);
	put_msg(&out, 'E', &body);
	put_msg(&out, 'S', &body);	/* Sync */
	write_full(fd, out.buf, out.len);

	while ((type = read_msg(fd, &msg, &mlen)) != 'Z')
	{
		if (type == 'D' && mlen > 6)
		{
			uint32_t	vl = ((unsigned char) msg[2] << 24 | (unsigned char) msg[3] << 16 |
							  (unsigned char) msg[4] << 8 | (unsigned char) msg[5]);

			snprintf(matched, sizeof(matched), "%.*s", (int) vl, msg + 6);
		}
		if (type == 'E' && !errbody)
		{
			errbody = msg;
			/* ErrorResponse: (field-type byte, C string)* terminated by a zero byte */
			for (char *p = msg; *p; p += strlen(p + 1) + 2)
			{
				if (*p == 'C')
					sqlstate = p + 1;
				else if (*p == 'M')
					message = p + 1;
			}
			continue;
		}
		free(msg);
	}
	free(msg);
	if (errbody)
		printf("wire n_params=%d bind_count_field=%u status=error sqlstate=%s message=\"%s\"\n",
			   n, count, sqlstate, message);
	else
		printf("wire n_params=%d bind_count_field=%u status=ok matched=%s\n", n, count, matched);
	close(fd);
}

int
main(int argc, char **argv)
{
	PGconn	   *conn;
	const char *mode;
	int			n;

	if (argc < 4)
	{
		fprintf(stderr, "usage: %s CONNINFO lookup|insert|wall|bigarray N ...\n", argv[0]);
		return 2;
	}
	mode = argv[2];
	n = atoi(argv[3]);
	if (strcmp(mode, "wire") == 0 && argc == 6)
	{
		run_wire(argv[1], n, atoll(argv[4]), atoi(argv[5]));
		return 0;
	}
	conn = PQconnectdb(argv[1]);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "connection failed: %s", PQerrorMessage(conn));
		return 1;
	}
	if (strcmp(mode, "lookup") == 0 && argc == 7)
		run_lookup(conn, n, atoi(argv[4]), atoll(argv[5]), strtoull(argv[6], NULL, 10));
	else if (strcmp(mode, "insert") == 0 && argc == 5)
		run_insert(conn, n, atoi(argv[4]));
	else if (strcmp(mode, "wall") == 0 && argc == 5)
		run_wall(conn, n, atoll(argv[4]));
	else if (strcmp(mode, "bigarray") == 0 && argc == 5)
		run_bigarray(conn, n, atoll(argv[4]));
	else
	{
		fprintf(stderr, "bad arguments for mode %s\n", mode);
		return 2;
	}
	PQfinish(conn);
	return 0;
}
