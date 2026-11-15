// SPDX-License-Identifier: GPL-2.0
/*
 * lfu-stats: fsstats-style histograms and the largest directories, from a
 * scan of one or more Lustre MDTs with llapi_scan_device().
 *
 * The histograms and their CSV form follow fsstats 1.4.5
 * (https://github.com/adilger/fsstats) so the two can be compared directly.
 * Nothing walks the namespace: entries per directory are counted from each
 * object's linkea, and paths are composed from the directories' own linkea.
 */
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <lustre/lustreapi.h>

#define FSSTATS_SEQ_ROOT	0x200000007ULL
#define FSSTATS_SEQ_DOT_LUSTRE	0x200000002ULL
#define LINKEA_MAGIC		0x11EAF1DFU
#define NBUCKET			80

/* ---------------------------------------------------------------- */
/* histograms, bucketed exactly as fsstats' Histo class buckets */

struct histo {
	double	min, max, incr;
	int	has_max, log_incr, integer_vals;
	double	count, total_val;
	double	min_val, max_val;
	int	have_val;
	double	under_min, under_val, over_max, over_val;
	double	b[NBUCKET], bv[NBUCKET];
	int	nb;
};

enum {
	H_SIZE, H_CAP, H_POS_OVHD, H_NEG_OVHD, H_DIR_ENTS, H_DIR_KB,
	H_FNAME, H_HLINK, H_SLINK,
	H_MTIME_F, H_MTIME_B, H_CTIME_F, H_CTIME_B, H_ATIME_F, H_ATIME_B,
	H_NR
};

static const struct {
	const char *name, *unit, *title;
	double max, incr;
	int has_max, log_incr, integer_vals;
} hdef[H_NR] = {
	[H_SIZE]     = { "file size", "KB", "file size", 0, 1, 0, 1, 0 },
	[H_CAP]	     = { "capacity used", "KB", "capacity used", 0, 1, 0, 1, 0 },
	[H_POS_OVHD] = { "positive overhead", "KB", "positive overhead", 0, 1, 0, 1, 0 },
	[H_NEG_OVHD] = { "negative overhead", "KB", "negative overhead", 0, 1, 0, 1, 0 },
	[H_DIR_ENTS] = { "directory size (entries)", "ents", "directory size (entries)", 0, 1, 0, 1, 1 },
	[H_DIR_KB]   = { "directory size", "KB", "directory size (KB)", 0, 1, 0, 1, 0 },
	[H_FNAME]    = { "filename length", "chars", "filename length", 127, 8, 1, 0, 1 },
	[H_HLINK]    = { "link count", "links", "link count", 0, 1, 0, 1, 1 },
	[H_SLINK]    = { "symlink target length", "chars", "symlink target length", 127, 8, 1, 0, 1 },
	[H_MTIME_F]  = { "mtime (files)", "days", "mtime age (files)", 0, 1, 0, 1, 0 },
	[H_MTIME_B]  = { "mtime (KB)", "days", "mtime age (KB)", 0, 1, 0, 1, 0 },
	[H_CTIME_F]  = { "ctime (files)", "days", "ctime age (files)", 0, 1, 0, 1, 0 },
	[H_CTIME_B]  = { "ctime (KB)", "days", "ctime age (KB)", 0, 1, 0, 1, 0 },
	[H_ATIME_F]  = { "atime (files)", "days", "atime age (files)", 0, 1, 0, 1, 0 },
	[H_ATIME_B]  = { "atime (KB)", "days", "atime age (KB)", 0, 1, 0, 1, 0 },
};

static void histo_init(struct histo *h, int i)
{
	memset(h, 0, sizeof(*h));
	h->max = hdef[i].max;
	h->has_max = hdef[i].has_max;
	h->incr = hdef[i].incr;
	h->log_incr = hdef[i].log_incr;
	h->integer_vals = hdef[i].integer_vals;
}

/* @key picks the bucket; fsstats buckets overhead by the file's size */
static void histo_add_key(struct histo *h, double key, double val, double cnt)
{
	int b;

	if (!h->have_val || val < h->min_val)
		h->min_val = val;
	if (!h->have_val || val > h->max_val)
		h->max_val = val;
	h->have_val = 1;
	h->count += cnt;
	h->total_val += val * cnt;

	if (h->has_max && val > h->max) {
		h->over_max += cnt;
		h->over_val += val * cnt;
		return;
	}
	if (val < h->min) {
		h->under_min += cnt;
		h->under_val += val * cnt;
		return;
	}
	if (h->log_incr) {
		double x = h->min + h->log_incr;

		for (b = 0; key >= x + 1 && b < NBUCKET - 1; b++)
			x = x * 2 + 1;
	} else {
		b = (int)((key - h->min) / h->incr);
		if (b >= NBUCKET)
			b = NBUCKET - 1;
	}
	h->b[b] += cnt;
	h->bv[b] += val * cnt;
	if (b + 1 > h->nb)
		h->nb = b + 1;
}

static void histo_add(struct histo *h, double val, double cnt)
{
	histo_add_key(h, val, val, cnt);
}

static void histo_merge(struct histo *d, const struct histo *s)
{
	int b;

	if (s->have_val) {
		if (!d->have_val || s->min_val < d->min_val)
			d->min_val = s->min_val;
		if (!d->have_val || s->max_val > d->max_val)
			d->max_val = s->max_val;
		d->have_val = 1;
	}
	d->count += s->count;
	d->total_val += s->total_val;
	d->under_min += s->under_min;
	d->under_val += s->under_val;
	d->over_max += s->over_max;
	d->over_val += s->over_val;
	for (b = 0; b < s->nb; b++) {
		d->b[b] += s->b[b];
		d->bv[b] += s->bv[b];
	}
	if (s->nb > d->nb)
		d->nb = s->nb;
}

static double bucket_min(const struct histo *h, int b)
{
	if (h->log_incr)
		return b == 0 ? h->min : ldexp(1, b);
	return h->min + h->incr * b;
}

static double bucket_max(const struct histo *h, int b)
{
	if (h->log_incr)
		return ldexp(1, b + 1) - 1;
	return h->incr * (b + 1) - 1;
}

static void histo_csv(FILE *f, const struct histo *h, int i)
{
	double c = 0, d = 0;
	int b;

	fprintf(f, "histogram,%s\n", hdef[i].name);
	fprintf(f, "count,%lld,items\n", (long long)h->count);
	fprintf(f, "average,%f,%s\n",
		h->count > 0 ? h->total_val / h->count : 0, hdef[i].unit);
	fprintf(f, "min,%lld,%s\n", (long long)h->min_val, hdef[i].unit);
	fprintf(f, "max,%lld,%s\n", (long long)h->max_val, hdef[i].unit);
	fprintf(f, "bucket min,bucket max,count,percent,cumulative pct,val count,percent,cumulative pct\n");
	if (h->under_min > 0) {
		c += h->under_min;
		d += h->under_val;
		fprintf(f, "%d,%lld,%lld,%f,%f,%f,%f,%f\n", -1,
			(long long)h->min, (long long)c, c / h->count,
			c / h->count, d, d / h->total_val, d / h->total_val);
	}
	for (b = 0; b < h->nb; b++) {
		double x = h->b[b], y = h->bv[b];

		if (x == 0)
			continue;
		c += x;
		d += y;
		fprintf(f, "%lld,%lld,%lld,%f,%f,%f,%f,%f\n",
			(long long)bucket_min(h, b),
			(long long)bucket_max(h, b) + !h->integer_vals,
			(long long)x, x / h->count, c / h->count, y,
			h->total_val ? y / h->total_val : 0,
			h->total_val ? d / h->total_val : 0);
	}
	if (h->over_max > 0) {
		c += h->over_max;
		d += h->over_val;
		fprintf(f, "%lld,%d,%lld,%f,%f,%f,%f,%f\n",
			(long long)h->max, -1, (long long)h->over_max,
			h->over_max / h->count, c / h->count, h->over_val,
			h->over_val / h->total_val, d / h->total_val);
	}
	fprintf(f, "\n");
}

static const char *kb_str(double kb, char *buf, size_t len)
{
	static const char *u[] = { "KB", "MB", "GB", "TB", "PB" };
	int i = 0;

	while (kb > 1024 && i < 4) {
		kb /= 1024;
		i++;
	}
	snprintf(buf, len, "%.2f %s", kb, u[i]);
	return buf;
}

/* ---------------------------------------------------------------- */
/* job_stats-style histograms for the YAML report */

enum { YK_LOG2, YK_AGE };

enum {
	Y_SIZE, Y_ALLOC, Y_OVHD, Y_SPARSE, Y_DIR_ENTS, Y_DIR_SIZE, Y_NAME,
	Y_LINKS, Y_SLINK, Y_MTIME, Y_CTIME, Y_ATIME, Y_NR
};

static const struct {
	const char *name, *unit;
	int kind, weighted;
} ydef[Y_NR] = {
	[Y_SIZE]     = { "file_size", "bytes", YK_LOG2, 1 },
	[Y_ALLOC]    = { "file_allocated", "bytes", YK_LOG2, 1 },
	[Y_OVHD]     = { "file_overhead", "bytes", YK_LOG2, 1 },
	[Y_SPARSE]   = { "file_sparse", "bytes", YK_LOG2, 1 },
	[Y_DIR_ENTS] = { "dir_entries", "entries", YK_LOG2, 0 },
	[Y_DIR_SIZE] = { "dir_size", "bytes", YK_LOG2, 0 },
	[Y_NAME]     = { "name_length", "chars", YK_LOG2, 0 },
	[Y_LINKS]    = { "file_links", "links", YK_LOG2, 0 },
	[Y_SLINK]    = { "symlink_length", "chars", YK_LOG2, 0 },
	[Y_MTIME]    = { "mtime_age", "secs", YK_AGE, 1 },
	[Y_CTIME]    = { "ctime_age", "secs", YK_AGE, 1 },
	[Y_ATIME]    = { "atime_age", "secs", YK_AGE, 1 },
};

/* age buckets: each label is the bucket's lower bound */
static const struct {
	int64_t secs;
	const char *label;
} age_bucket[] = {
	{ 0, "0" }, { 3600, "1h" }, { 86400, "1d" }, { 604800, "1w" },
	{ 2592000, "1M" }, { 7776000, "3M" }, { 15552000, "6M" },
	{ 31536000, "1y" }, { 63072000, "2y" }, { 157680000, "5y" },
	{ 315360000, "10y" },
};
#define AGE_NB	(sizeof(age_bucket) / sizeof(age_bucket[0]))

#define YBUCKET	66	/* log2: 0, then [2^(n-1), 2^n) for n = 1..64 */

struct yhist {
	uint64_t	samples;
	int64_t		min, max, sum;
	uint64_t	b[YBUCKET], bb[YBUCKET];	/* count, bytes */
};

static int ybucket(int kind, int64_t v)
{
	unsigned int i;

	if (kind == YK_LOG2)
		return v <= 0 ? 0 : 64 - __builtin_clzll((uint64_t)v);
	if (v < 0)
		return 0;		/* in the future */
	for (i = 1; i < AGE_NB && v >= age_bucket[i].secs; i++)
		;
	return i;		/* 1 .. AGE_NB */
}

static void yh_add(struct yhist *h, int kind, int64_t v, uint64_t bytes)
{
	int b = ybucket(kind, v);

	if (h->samples == 0 || v < h->min)
		h->min = v;
	if (h->samples == 0 || v > h->max)
		h->max = v;
	h->samples++;
	h->sum += v;
	h->b[b]++;
	h->bb[b] += bytes;
}

static void yh_merge(struct yhist *d, const struct yhist *s)
{
	int b;

	if (s->samples == 0)
		return;
	if (d->samples == 0 || s->min < d->min)
		d->min = s->min;
	if (d->samples == 0 || s->max > d->max)
		d->max = s->max;
	d->samples += s->samples;
	d->sum += s->sum;
	for (b = 0; b < YBUCKET; b++) {
		d->b[b] += s->b[b];
		d->bb[b] += s->bb[b];
	}
}

static const char *ylabel(int kind, int b, char *buf, size_t len)
{
	static const char sfx[] = "KMGTPE";
	uint64_t x;
	int k = -1;

	if (kind == YK_AGE)
		return b == 0 ? "future" : age_bucket[b - 1].label;
	if (b == 0)
		return "0";
	x = 1ULL << (b - 1);
	while (x >= 1024 && k < 5) {
		x >>= 10;
		k++;
	}
	if (k < 0)
		snprintf(buf, len, "%"PRIu64, x);
	else
		snprintf(buf, len, "%"PRIu64"%c", x, sfx[k]);
	return buf;
}

static void yh_print_map(FILE *f, const struct yhist *h, int kind,
			 const uint64_t *v)
{
	const char *sep = "";
	char buf[32];
	int b;

	fprintf(f, "{ ");
	for (b = 0; b < YBUCKET; b++) {
		if (h->b[b] == 0)
			continue;
		fprintf(f, "%s%s: %"PRIu64, sep, ylabel(kind, b, buf,
			sizeof(buf)), v[b]);
		sep = ", ";
	}
	fprintf(f, " }");
}

static void yh_print(FILE *f, const struct yhist *h, int i)
{
	char key[32];

	snprintf(key, sizeof(key), "%s:", ydef[i].name);
	fprintf(f, "  %-17s { samples: %"PRIu64", unit: %s, min: %"PRId64
		", max: %"PRId64", sum: %"PRId64", hist: ",
		key, h->samples, ydef[i].unit, h->min, h->max,
		h->sum);
	yh_print_map(f, h, ydef[i].kind, h->b);
	if (ydef[i].weighted) {
		fprintf(f, ", hist_bytes: ");
		yh_print_map(f, h, ydef[i].kind, h->bb);
	}
	fprintf(f, " }\n");
}

/* a YAML double-quoted scalar; bytes from 0x80 up are passed as UTF-8 */
static void yaml_str(FILE *f, const char *str)
{
	const unsigned char *c;

	fputc('"', f);
	for (c = (const unsigned char *)str; *c != '\0'; c++) {
		if (*c == '"' || *c == '\\')
			fprintf(f, "\\%c", *c);
		else if (*c < 0x20 || *c == 0x7f)
			fprintf(f, "\\x%02x", *c);
		else
			fputc(*c, f);
	}
	fputc('"', f);
}

/* ---------------------------------------------------------------- */
/* directory table: one entry per directory FID, open addressing */

#define D_SEEN	0x1	/* the directory object itself was scanned */
#define D_SHARD	0x2	/* a stripe of a striped directory; parent is the master */
#define D_STRIPED 0x4	/* a striped directory: size and blocks are its shards' */

struct dent {
	struct lu_fid	fid;
	struct lu_fid	parent;	/* first linkea parent of the directory */
	char		*name;	/* first linkea name of the directory */
	uint64_t	nent;	/* linkea entries naming this as parent */
	uint64_t	bytes;	/* file sizes directly beneath, first link */
	uint64_t	dsize;	/* directory inode size, shards folded in */
	uint64_t	dblocks; /* a striped directory's shards' blocks */
	uint32_t	flags;
};

struct dtab {
	struct dent	*e;
	size_t		cap, used;
};

static inline uint64_t fid_hash(const struct lu_fid *f)
{
	uint64_t h = f->f_seq * 0x9E3779B97F4A7C15ULL;

	h ^= ((uint64_t)f->f_oid << 32 | f->f_ver) * 0xC2B2AE3D27D4EB4FULL;
	return h ^ (h >> 29);
}

static inline int fid_eq(const struct lu_fid *a, const struct lu_fid *b)
{
	return a->f_seq == b->f_seq && a->f_oid == b->f_oid &&
	       a->f_ver == b->f_ver;
}

static struct dent *dtab_slot(struct dtab *t, const struct lu_fid *fid);

static void dtab_grow(struct dtab *t)
{
	struct dtab n = { .cap = t->cap ? t->cap * 2 : 1024 };
	size_t i;

	n.e = calloc(n.cap, sizeof(*n.e));
	if (n.e == NULL) {
		perror("lfu-stats: directory table");
		exit(1);
	}
	for (i = 0; i < t->cap; i++) {
		if (t->e[i].fid.f_seq == 0)
			continue;
		*dtab_slot(&n, &t->e[i].fid) = t->e[i];
		n.used++;
	}
	free(t->e);
	*t = n;
}

/* the slot for @fid, or the empty one it would go in */
static struct dent *dtab_slot(struct dtab *t, const struct lu_fid *fid)
{
	size_t i = fid_hash(fid) & (t->cap - 1);

	while (t->e[i].fid.f_seq != 0 && !fid_eq(&t->e[i].fid, fid))
		i = (i + 1) & (t->cap - 1);
	return &t->e[i];
}

static struct dent *dtab_get(struct dtab *t, const struct lu_fid *fid)
{
	struct dent *d;

	if (t->used * 2 >= t->cap)
		dtab_grow(t);
	d = dtab_slot(t, fid);
	if (d->fid.f_seq == 0) {
		d->fid = *fid;
		t->used++;
	}
	return d;
}

static struct dent *dtab_find(const struct dtab *t, const struct lu_fid *fid)
{
	struct dent *d;

	if (t->cap == 0)
		return NULL;
	d = dtab_slot((struct dtab *)t, fid);
	return d->fid.f_seq == 0 ? NULL : d;
}

static void dent_merge(struct dent *d, const struct dent *s)
{
	d->nent += s->nent;
	d->bytes += s->bytes;
	d->dsize += s->dsize;
	d->dblocks += s->dblocks;
	if (s->flags & D_SHARD)
		d->parent = s->parent;
	if ((s->flags & D_SEEN) && !(d->flags & D_SEEN)) {
		d->parent = s->parent;
		d->name = s->name;
	} else if (s->name != NULL && s->name != d->name) {
		free(s->name);
	}
	d->flags |= s->flags;
}

/* ---------------------------------------------------------------- */
/* OST pass: blocks per owning MDT file, from each object's trusted.fid */

struct oent {
	struct lu_fid	fid;	/* the MDT file owning the objects */
	uint64_t	blocks;	/* 512-byte blocks, all its objects */
	uint32_t	nobj;
	uint32_t	matched; /* a scanned MDT file took these blocks */
};

struct otab {
	struct oent	*e;
	size_t		cap, used;
};

static struct oent *otab_slot(const struct otab *t, const struct lu_fid *fid)
{
	size_t i = fid_hash(fid) & (t->cap - 1);

	while (t->e[i].fid.f_seq != 0 && !fid_eq(&t->e[i].fid, fid))
		i = (i + 1) & (t->cap - 1);
	return &t->e[i];
}

static struct oent *otab_get(struct otab *t, const struct lu_fid *fid)
{
	struct oent *o;

	if (t->used * 2 >= t->cap) {
		struct otab n = { .cap = t->cap ? t->cap * 2 : 1024 };
		size_t i;

		n.e = calloc(n.cap, sizeof(*n.e));
		if (n.e == NULL) {
			perror("lfu-stats: owner table");
			exit(1);
		}
		for (i = 0; i < t->cap; i++)
			if (t->e[i].fid.f_seq != 0)
				*otab_slot(&n, &t->e[i].fid) = t->e[i];
		n.used = t->used;
		free(t->e);
		*t = n;
	}
	o = otab_slot(t, fid);
	if (o->fid.f_seq == 0) {
		o->fid = *fid;
		t->used++;
	}
	return o;
}

static struct oent *otab_find(const struct otab *t, const struct lu_fid *fid)
{
	struct oent *o;

	if (t->cap == 0)
		return NULL;
	o = otab_slot(t, fid);
	return o->fid.f_seq == 0 ? NULL : o;
}

/* filled by the OST scans, then only read (and marked) by the MDT scans */
static struct otab ost_all;
static int have_ost;

/* ---------------------------------------------------------------- */
/* per-thread state, merged when the scans end */

struct tstate {
	struct histo	h[H_NR];
	struct dtab	dirs;
	double		total_kb, total_cap_kb;
	uint64_t	special, hlink_skipped, files, dirs_seen, symlinks;
	uint64_t	shards, no_name, size_strict, size_lazy, size_none;
	struct yhist	y[Y_NR];
	/* OST pass */
	struct otab	owners;
	uint64_t	ost_objs, ost_unowned, ost_blocks;
	uint64_t	cap_ost, cap_lazy, dom_plus_ost;
	struct tstate	*next;
};

static pthread_mutex_t ts_lock = PTHREAD_MUTEX_INITIALIZER;
static struct tstate *ts_list;
static __thread struct tstate *ts_mine;
static time_t now;

static struct tstate *ts_get(void)
{
	struct tstate *t = ts_mine;
	int i;

	if (t != NULL)
		return t;
	t = calloc(1, sizeof(*t));
	if (t == NULL) {
		perror("lfu-stats");
		exit(1);
	}
	for (i = 0; i < H_NR; i++)
		histo_init(&t->h[i], i);
	pthread_mutex_lock(&ts_lock);
	t->next = ts_list;
	ts_list = t;
	pthread_mutex_unlock(&ts_lock);
	ts_mine = t;
	return t;
}

/* ---------------------------------------------------------------- */
/* linkea, as linkea_entry_unpack() reads it: big-endian entries */

struct lee_raw {
	unsigned char	reclen[2];
	unsigned char	pfid[16];
	char		name[];
};

static int linkea_next(const void *buf, size_t len, size_t *off,
		       struct lu_fid *parent, const char **name, int *namelen)
{
	const struct lee_raw *e;
	const unsigned char *p;
	int reclen;

	if (*off == 0) {
		uint32_t magic;

		if (len < 24)
			return 0;
		memcpy(&magic, buf, 4);
		if (magic != LINKEA_MAGIC && magic != __builtin_bswap32(LINKEA_MAGIC))
			return 0;
		*off = 24;
	}
	if (*off + sizeof(*e) > len)
		return 0;
	e = (const struct lee_raw *)((const char *)buf + *off);
	reclen = e->reclen[0] << 8 | e->reclen[1];
	if (reclen <= (int)sizeof(*e) || *off + reclen > len)
		return 0;
	p = e->pfid;
	parent->f_seq = (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 |
			(uint64_t)p[2] << 40 | (uint64_t)p[3] << 32 |
			(uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 |
			(uint64_t)p[6] << 8 | p[7];
	parent->f_oid = (uint32_t)p[8] << 24 | p[9] << 16 | p[10] << 8 | p[11];
	parent->f_ver = (uint32_t)p[12] << 24 | p[13] << 16 | p[14] << 8 | p[15];
	*name = e->name;
	*namelen = reclen - sizeof(*e);
	*off += reclen;
	return 1;
}

/* ---------------------------------------------------------------- */

static int ost_filter(const struct llapi_scan_rec *rec, void *data)
{
	return (rec->lfsr_valid & LLAPI_SCAN_CLASS) &&
	       rec->lfsr_class != LLAPI_SCAN_CLS_OST_OBJ;
}

static int ost_cb(const struct llapi_scan_rec *rec, void *data)
{
	struct tstate *t = ts_get();
	uint64_t blocks = (rec->lfsr_stx.stx_mask & STATX_BLOCKS) ?
			  rec->lfsr_stx.stx_blocks : 0;
	struct oent *o;

	t->ost_objs++;
	t->ost_blocks += blocks;
	/* precreated and never written: no owner, and no data */
	if (!(rec->lfsr_valid & LLAPI_SCAN_OWNER)) {
		t->ost_unowned++;
		return 0;
	}
	o = otab_get(&t->owners, &rec->lfsr_owner_fid);
	o->blocks += blocks;
	o->nobj++;
	return 0;
}

/*
 * The end of an instantiated Data-on-MDT component, from the raw
 * trusted.lov, or 0 if there is none.  A file with data past it on OSTs
 * has written that whole component, so its MDT blocks are about this many
 * bytes, unless the component has holes.
 */
static uint64_t lov_dom_end(const void *buf, size_t len)
{
	const struct lov_comp_md_v1 *comp = buf;
	const struct lov_user_md_v1 *v1;
	unsigned int i, count;

	if (buf == NULL || len < sizeof(*comp) ||
	    comp->lcm_magic != LOV_USER_MAGIC_COMP_V1)
		return 0;
	count = comp->lcm_entry_count;
	if (len < sizeof(*comp) + count * sizeof(comp->lcm_entries[0]))
		return 0;
	for (i = 0; i < count; i++) {
		uint32_t off = comp->lcm_entries[i].lcme_offset;

		if (!(comp->lcm_entries[i].lcme_flags & LCME_FL_INIT) ||
		    off > len - sizeof(*v1))
			continue;
		v1 = (const struct lov_user_md_v1 *)((const char *)buf + off);
		/* a DoM component ends before any OST one: EOF is not a size */
		if ((v1->lmm_pattern & LOV_PATTERN_MDT) &&
		    comp->lcm_entries[i].lcme_extent.e_end != LUSTRE_EOF)
			return comp->lcm_entries[i].lcme_extent.e_end;
	}
	return 0;
}

static int filter(const struct llapi_scan_rec *rec, void *data)
{
	/* only what the namespace can reach: no agents, orphans, internals */
	if ((rec->lfsr_valid & LLAPI_SCAN_CLASS) &&
	    rec->lfsr_class != LLAPI_SCAN_CLS_VISIBLE)
		return 1;
	if ((rec->lfsr_valid & LLAPI_SCAN_FID) &&
	    rec->lfsr_fid.f_seq == FSSTATS_SEQ_DOT_LUSTRE)
		return 1;
	return 0;
}

static double age_days(int64_t sec)
{
	return (double)(now - sec) / 86400;
}

static int scan_cb(const struct llapi_scan_rec *rec, void *data)
{
	const lstatx_t *st = &rec->lfsr_stx;
	struct tstate *t = ts_get();
	struct lu_fid parent, first_parent = { 0 };
	const char *name, *first_name = NULL;
	int namelen, first_namelen = 0, nlinks = 0;
	size_t off = 0;
	mode_t type = st->stx_mode & S_IFMT;
	int has_size, is_shard;
	double size_kb, cap_kb;
	struct dent *d;

	if (rec->lfsr_fid.f_seq == FSSTATS_SEQ_DOT_LUSTRE)
		return 0;
	is_shard = !!(rec->lfsr_valid & LLAPI_SCAN_LMV_SHARD);

	/* every name of the object: name length, and an entry of its parent */
	while (!is_shard && (rec->lfsr_valid & LLAPI_SCAN_LINKEA) &&
	       linkea_next(rec->lfsr_linkea, rec->lfsr_linkeasize, &off,
			   &parent, &name, &namelen)) {
		if (parent.f_seq == FSSTATS_SEQ_DOT_LUSTRE)
			continue;
		if (nlinks++ == 0) {
			first_parent = parent;
			first_name = name;
			first_namelen = namelen;
		}
		histo_add(&t->h[H_FNAME], namelen, 1);
		yh_add(&t->y[Y_NAME], YK_LOG2, namelen, 0);
		dtab_get(&t->dirs, &parent)->nent++;
	}
	if (nlinks == 0 && !is_shard &&
	    !(rec->lfsr_fid.f_seq == FSSTATS_SEQ_ROOT &&
	      rec->lfsr_fid.f_oid == 1))
		t->no_name++;

	if (is_shard) {
		/* a stripe of a directory: its size is part of the master's */
		off = 0;
		t->shards++;
		if ((rec->lfsr_valid & LLAPI_SCAN_LINKEA) &&
		    linkea_next(rec->lfsr_linkea, rec->lfsr_linkeasize, &off,
				&parent, &name, &namelen)) {
			d = dtab_get(&t->dirs, &parent);
			d->dsize += st->stx_size;
			d->dblocks += st->stx_blocks;
			/* entries name the shard as parent: folded at the end */
			d = dtab_get(&t->dirs, &rec->lfsr_fid);
			d->flags |= D_SHARD;
			d->parent = parent;
		}
		return 0;
	}

	has_size = (st->stx_mask & STATX_SIZE) ||
		   (rec->lfsr_valid & LLAPI_SCAN_LAZY_SIZE);
	size_kb = has_size ? st->stx_size / 1024.0 : 0;
	cap_kb = ((st->stx_mask & STATX_BLOCKS) ||
		  (rec->lfsr_valid & LLAPI_SCAN_LAZY_BLOCKS)) ?
		 st->stx_blocks / 2.0 : 0;
	if (type == S_IFREG && have_ost) {
		struct oent *o = otab_find(&ost_all, &rec->lfsr_fid);

		if (o != NULL) {
			/* what a client's stat sums: every object's blocks */
			cap_kb = o->blocks / 2.0;
			o->matched = 1;
			t->cap_ost++;
			if (rec->lfsr_valid & LLAPI_SCAN_LAYOUT) {
				uint64_t dom = lov_dom_end(rec->lfsr_lmm,
							   rec->lfsr_lmmsize);

				if (dom != 0) {
					cap_kb += dom / 1024.0;
					t->dom_plus_ost++;
				}
			}
		} else if (!(st->stx_mask & STATX_BLOCKS)) {
			t->cap_lazy++;
		}
	}

	/* a striped directory has no size of its own: counted at the end */
	if (type == S_IFDIR && (rec->lfsr_valid & LLAPI_SCAN_LMV) &&
	    !(rec->lfsr_valid & LLAPI_SCAN_LMV_FOREIGN) &&
	    rec->lfsr_lmv != NULL && rec->lfsr_lmv->lum_stripe_count > 1) {
		t->dirs_seen++;
		d = dtab_get(&t->dirs, &rec->lfsr_fid);
		d->flags |= D_SEEN | D_STRIPED;
		if (nlinks > 0) {
			d->parent = first_parent;
			d->name = strndup(first_name, first_namelen);
		}
		return 0;
	}

	if (type == S_IFREG || type == S_IFDIR || type == S_IFLNK) {
		double ovhd = cap_kb >= size_kb ? cap_kb - size_kb :
						  size_kb - cap_kb;

		histo_add_key(&t->h[cap_kb >= size_kb ? H_POS_OVHD : H_NEG_OVHD],
			      size_kb, ovhd, 1);
	}
	if (type == S_IFREG && nlinks > 1)
		t->hlink_skipped += nlinks - 1;
	t->total_kb += size_kb;
	t->total_cap_kb += cap_kb;

	switch (type) {
	case S_IFREG:
		t->files++;
		if (st->stx_mask & STATX_SIZE)
			t->size_strict++;
		else if (rec->lfsr_valid & LLAPI_SCAN_LAZY_SIZE)
			t->size_lazy++;
		else
			t->size_none++;
		histo_add(&t->h[H_SIZE], size_kb, 1);
		histo_add(&t->h[H_CAP], cap_kb, 1);
		histo_add(&t->h[H_HLINK], st->stx_nlink, 1);
		histo_add(&t->h[H_MTIME_F], age_days(st->stx_mtime.tv_sec), 1);
		histo_add(&t->h[H_MTIME_B], age_days(st->stx_mtime.tv_sec), size_kb);
		histo_add(&t->h[H_CTIME_F], age_days(st->stx_ctime.tv_sec), 1);
		histo_add(&t->h[H_CTIME_B], age_days(st->stx_ctime.tv_sec), size_kb);
		histo_add(&t->h[H_ATIME_F], age_days(st->stx_atime.tv_sec), 1);
		histo_add(&t->h[H_ATIME_B], age_days(st->stx_atime.tv_sec), size_kb);
		{
			uint64_t sz = has_size ? st->stx_size : 0;
			uint64_t al = (uint64_t)(cap_kb * 1024);

			yh_add(&t->y[Y_SIZE], YK_LOG2, sz, sz);
			yh_add(&t->y[Y_ALLOC], YK_LOG2, al, al);
			if (al >= sz)
				yh_add(&t->y[Y_OVHD], YK_LOG2, al - sz, al - sz);
			else
				yh_add(&t->y[Y_SPARSE], YK_LOG2, sz - al, sz - al);
			yh_add(&t->y[Y_LINKS], YK_LOG2, st->stx_nlink, 0);
			yh_add(&t->y[Y_MTIME], YK_AGE,
			       now - st->stx_mtime.tv_sec, sz);
			yh_add(&t->y[Y_CTIME], YK_AGE,
			       now - st->stx_ctime.tv_sec, sz);
			yh_add(&t->y[Y_ATIME], YK_AGE,
			       now - st->stx_atime.tv_sec, sz);
		}
		if (nlinks > 0)
			dtab_get(&t->dirs, &first_parent)->bytes +=
				st->stx_size;
		break;
	case S_IFDIR:
		t->dirs_seen++;
		d = dtab_get(&t->dirs, &rec->lfsr_fid);
		d->flags |= D_SEEN;
		d->dsize += st->stx_size;
		if (nlinks > 0) {
			d->parent = first_parent;
			d->name = strndup(first_name, first_namelen);
		}
		break;
	case S_IFLNK:
		t->symlinks++;
		histo_add(&t->h[H_SLINK], st->stx_size, 1);
		yh_add(&t->y[Y_SLINK], YK_LOG2, st->stx_size, 0);
		break;
	default:
		t->special++;
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* paths and the largest directories */

static int is_root(const struct lu_fid *f)
{
	return f->f_seq == FSSTATS_SEQ_ROOT && f->f_oid == 1 && f->f_ver == 0;
}

static void dir_path(const struct dtab *t, const struct dent *d,
		     char *buf, size_t len)
{
	const char *comp[256];
	const struct dent *p = d;
	char head[64] = "";
	size_t o;
	int n = 0;

	/* up the first-link parents to the root, or to a directory not seen */
	while (!is_root(&p->fid) && n < 256) {
		const struct dent *q;

		if (!(p->flags & D_SEEN) || p->name == NULL) {
			snprintf(head, sizeof(head), DFID, PFID(&p->fid));
			break;
		}
		comp[n++] = p->name;
		if (is_root(&p->parent))
			break;
		q = dtab_find(t, &p->parent);
		if (q != NULL && (q->flags & D_SHARD))
			q = dtab_find(t, &q->parent);
		if (q == NULL || !(q->flags & D_SEEN)) {
			snprintf(head, sizeof(head), DFID, PFID(&p->parent));
			break;
		}
		p = q;
	}
	o = snprintf(buf, len, "%s", head);
	if (n == 0 && head[0] == '\0')
		snprintf(buf, len, "/");
	while (n > 0 && o < len)
		o += snprintf(buf + o, len - o, "/%s", comp[--n]);
}

static int top_by;	/* 0 entries, 1 bytes */

static int dent_cmp(const void *a, const void *b)
{
	const struct dent *x = *(const struct dent * const *)a;
	const struct dent *y = *(const struct dent * const *)b;
	uint64_t vx = top_by ? x->bytes : x->nent;
	uint64_t vy = top_by ? y->bytes : y->nent;

	return vx < vy ? 1 : vx > vy ? -1 : 0;
}

static void print_top(FILE *f, const struct dtab *t, int n, int by)
{
	struct dent **v;
	size_t i, k = 0;
	char path[4096];

	v = malloc(t->used * sizeof(*v));
	if (v == NULL)
		return;
	for (i = 0; i < t->cap; i++)
		if (t->e[i].fid.f_seq != 0 && (t->e[i].flags & D_SEEN))
			v[k++] = &t->e[i];
	top_by = by;
	qsort(v, k, sizeof(*v), dent_cmp);
	fprintf(f, "  %s:\n", by ? "largest_dirs_by_bytes" :
				   "largest_dirs_by_entries");
	for (i = 0; i < k && (int)i < n; i++) {
		dir_path(t, v[i], path, sizeof(path));
		fprintf(f, "  - { entries: %"PRIu64", file_bytes: %"PRIu64
			", dir_size: %"PRIu64", path: ", v[i]->nent,
			v[i]->bytes, v[i]->dsize);
		yaml_str(f, path);
		fprintf(f, " }\n");
	}
	free(v);
}

/* ---------------------------------------------------------------- */

static void usage(const char *prog)
{
	fprintf(stderr,
"usage: %s [-t THREADS] [-n TOP] [-c CSVFILE] [--now EPOCH] [--ost OSTDEV]...\n"
"       MDTDEV...\n"
"  Scan each MDT (unmounted, or a snapshot) and print a YAML report of\n"
"  histograms and the largest TOP directories (default 20).  Several MDTs\n"
"  of one filesystem merge into one report.  With --ost, each OST given is\n"
"  scanned first and files take their capacity from their OST objects\n"
"  instead of the MDT's lazy size-on-MDT; give every OST for exact figures.\n"
"  -c writes the fsstats 1.4.5 histograms as CSV, for comparison with it.\n",
		prog);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "threads", required_argument, NULL, 't' },
		{ "top", required_argument, NULL, 'n' },
		{ "csv", required_argument, NULL, 'c' },
		{ "now", required_argument, NULL, 'N' },
		{ "ost", required_argument, NULL, 'O' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL }
	};
	struct tstate all = { 0 }, *t;
	int threads = 4, top = 20, i, c, rc = 0;
	const char *csv = NULL, **osts = NULL;
	int nost = 0;
	uint64_t unmatched_objs = 0, unmatched_blocks = 0;
	struct timespec t0, t1;
	uint64_t seen = 0, emitted = 0, skipped = 0;
	char b1[32];
	struct rusage ru;
	double secs;
	size_t j;

	now = time(NULL);
	osts = calloc(argc, sizeof(*osts));
	if (osts == NULL)
		return 1;
	while ((c = getopt_long(argc, argv, "t:n:c:O:h", opts, NULL)) != -1) {
		switch (c) {
		case 't': threads = atoi(optarg); break;
		case 'n': top = atoi(optarg); break;
		case 'c': csv = optarg; break;
		case 'N': now = atoll(optarg); break;
		case 'O': osts[nost++] = optarg; break;
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}
	if (optind >= argc || threads < 1 || threads > 255) {
		usage(argv[0]);
		return 2;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < nost; i++) {
		struct llapi_scan_stats ss = { .ss_size = sizeof(ss) };
		__u64 got = 0;
		struct llapi_scan_param sp = {
			.lfsp_size = sizeof(sp),
			.lfsp_want = LLAPI_SCAN_BLOCKS | LLAPI_SCAN_CLASS |
				     LLAPI_SCAN_OWNER,
			.lfsp_filter = ost_filter,
			.lfsp_thread_count = threads,
			.lfsp_got = &got,
			.lfsp_stats = &ss,
		};

		rc = llapi_scan_device(osts[i], &sp, ost_cb, NULL);
		if (rc < 0) {
			fprintf(stderr, "lfu-stats: %s: %s\n", osts[i],
				strerror(-rc));
			return 1;
		}
		if (!(got & LLAPI_SCAN_OWNER)) {
			fprintf(stderr, "lfu-stats: %s: not an OST\n", osts[i]);
			return 1;
		}
		seen += ss.ss_seen;
		emitted += ss.ss_emitted;
		skipped += ss.ss_skipped;
	}
	/* one owner table, the largest thread's adopted, read by the MDT pass */
	for (t = ts_list; t != NULL; t = t->next)
		if (t->owners.used > ost_all.used) {
			struct otab x = ost_all;

			ost_all = t->owners;
			t->owners = x;
		}
	for (t = ts_list; t != NULL; t = t->next) {
		for (j = 0; j < t->owners.cap; j++) {
			struct oent *s = &t->owners.e[j], *o;

			if (s->fid.f_seq == 0)
				continue;
			o = otab_get(&ost_all, &s->fid);
			o->blocks += s->blocks;
			o->nobj += s->nobj;
		}
		free(t->owners.e);
		t->owners = (struct otab){ 0 };
	}
	have_ost = nost > 0;

	for (i = optind; i < argc; i++) {
		struct llapi_scan_stats ss = { .ss_size = sizeof(ss) };
		__u64 got = 0;
		struct llapi_scan_param sp = {
			.lfsp_size = sizeof(sp),
			.lfsp_want = LLAPI_SCAN_TYPE | LLAPI_SCAN_MODE |
				     LLAPI_SCAN_NLINK | LLAPI_SCAN_SIZE |
				     LLAPI_SCAN_BLOCKS | LLAPI_SCAN_LAZY_SIZE |
				     LLAPI_SCAN_LAZY_BLOCKS |
				     LLAPI_SCAN_ATIME | LLAPI_SCAN_MTIME |
				     LLAPI_SCAN_CTIME | LLAPI_SCAN_FID |
				     LLAPI_SCAN_CLASS | LLAPI_SCAN_LINKEA |
				     LLAPI_SCAN_LMV |
				     (have_ost ? LLAPI_SCAN_LAYOUT : 0),
			.lfsp_filter = filter,
			.lfsp_thread_count = threads,
			.lfsp_got = &got,
			.lfsp_stats = &ss,
		};

		rc = llapi_scan_device(argv[i], &sp, scan_cb, NULL);
		if (rc < 0) {
			fprintf(stderr, "lfu-stats: %s: %s\n", argv[i],
				strerror(-rc));
			return 1;
		}
		if ((got & LLAPI_SCAN_OWNER) || !(got & LLAPI_SCAN_LMV)) {
			fprintf(stderr, "lfu-stats: %s: not an MDT%s\n", argv[i],
				(got & LLAPI_SCAN_OWNER) ? "; give an OST with --ost" : "");
			return 1;
		}
		seen += ss.ss_seen;
		emitted += ss.ss_emitted;
		skipped += ss.ss_skipped;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	/* merge the threads' state */
	for (i = 0; i < H_NR; i++)
		histo_init(&all.h[i], i);
	/* merge into the largest thread table, not a copy of it */
	for (t = ts_list; t != NULL; t = t->next)
		if (t->dirs.used > all.dirs.used) {
			struct dtab x = all.dirs;

			all.dirs = t->dirs;
			t->dirs = x;
		}
	for (t = ts_list; t != NULL; t = t->next) {
		for (i = 0; i < H_NR; i++)
			histo_merge(&all.h[i], &t->h[i]);
		for (i = 0; i < Y_NR; i++)
			yh_merge(&all.y[i], &t->y[i]);
		all.total_kb += t->total_kb;
		all.total_cap_kb += t->total_cap_kb;
		all.special += t->special;
		all.hlink_skipped += t->hlink_skipped;
		all.files += t->files;
		all.dirs_seen += t->dirs_seen;
		all.symlinks += t->symlinks;
		all.shards += t->shards;
		all.no_name += t->no_name;
		all.size_strict += t->size_strict;
		all.size_lazy += t->size_lazy;
		all.size_none += t->size_none;
		all.ost_objs += t->ost_objs;
		all.ost_unowned += t->ost_unowned;
		all.ost_blocks += t->ost_blocks;
		all.cap_ost += t->cap_ost;
		all.cap_lazy += t->cap_lazy;
		all.dom_plus_ost += t->dom_plus_ost;
		for (j = 0; j < t->dirs.cap; j++)
			if (t->dirs.e[j].fid.f_seq != 0)
				dent_merge(dtab_get(&all.dirs,
						    &t->dirs.e[j].fid),
					   &t->dirs.e[j]);
		free(t->dirs.e);
	}
	/* a shard's entries are its master's; the master is in the table
	 * already, its dsize having been added when the shard was seen
	 */
	for (j = 0; j < all.dirs.cap; j++) {
		struct dent *s = &all.dirs.e[j], *m;

		if (s->fid.f_seq == 0 || !(s->flags & D_SHARD))
			continue;
		m = dtab_find(&all.dirs, &s->parent);
		if (m == NULL)
			continue;
		m->nent += s->nent;
		m->bytes += s->bytes;
		s->nent = s->bytes = 0;
	}
	for (j = 0; j < ost_all.cap; j++)
		if (ost_all.e[j].fid.f_seq != 0 && !ost_all.e[j].matched) {
			unmatched_objs += ost_all.e[j].nobj;
			unmatched_blocks += ost_all.e[j].blocks;
		}
	/* fsstats counts every directory it walks, entries or not */
	for (j = 0; j < all.dirs.cap; j++) {
		struct dent *d = &all.dirs.e[j];

		if (d->fid.f_seq == 0 || !(d->flags & D_SEEN))
			continue;
		histo_add(&all.h[H_DIR_ENTS], d->nent, 1);
		histo_add(&all.h[H_DIR_KB], d->dsize / 1024.0, 1);
		yh_add(&all.y[Y_DIR_ENTS], YK_LOG2, d->nent, 0);
		yh_add(&all.y[Y_DIR_SIZE], YK_LOG2, d->dsize, 0);
		if (d->flags & D_STRIPED) {
			double size_kb = d->dsize / 1024.0;
			double cap_kb = d->dblocks / 2.0;

			histo_add_key(&all.h[cap_kb >= size_kb ? H_POS_OVHD :
					     H_NEG_OVHD], size_kb,
				      fabs(cap_kb - size_kb), 1);
			all.total_kb += size_kb;
			all.total_cap_kb += cap_kb;
		}
	}

	if (csv != NULL) {
		FILE *f = strcmp(csv, "-") ? fopen(csv, "w") : stdout;

		if (f == NULL) {
			fprintf(stderr, "lfu-stats: %s: %s\n", csv,
				strerror(errno));
			return 1;
		}
		fprintf(f, "#Generated by lfu-stats (fsstats 1.4.5 CSV)\n\n");
		fprintf(f, "skipped special files,%"PRIu64"\n", all.special);
		fprintf(f, "skipped duplicate hardlinks,%"PRIu64"\n",
			all.hlink_skipped);
		fprintf(f, "skipped snapshot dirs,0\n");
		fprintf(f, "total capacity used,%s\n",
			kb_str(all.total_cap_kb, b1, sizeof(b1)));
		fprintf(f, "total user data,%s\n",
			kb_str(all.total_kb, b1, sizeof(b1)));
		fprintf(f, "percent overhead,%f\n", all.total_cap_kb ?
			(all.total_cap_kb - all.total_kb) /
			all.total_cap_kb : 0);
		fprintf(f, "\n");
		for (i = 0; i < H_NR; i++) {
			histo_csv(f, &all.h[i], i);
			/* symlink targets are not read by a scan */
			if (i == H_SLINK)
				fprintf(f, "relative symlink target pct,n/a\n"
					"absolute symlink target pct,n/a\n");
		}
		if (f != stdout)
			fclose(f);
	}

	printf("scan_stats:\n");
	printf("- %-17s [ ", "mdts:");
	for (i = optind; i < argc; i++) {
		yaml_str(stdout, argv[i]);
		printf(i + 1 < argc ? ", " : " ]\n");
	}
	if (have_ost) {
		printf("  %-17s [ ", "osts:");
		for (i = 0; i < nost; i++) {
			yaml_str(stdout, osts[i]);
			printf(i + 1 < nost ? ", " : " ]\n");
		}
	}
	printf("  %-17s %lld\n", "snapshot_time:", (long long)now);
	printf("  %-17s %.3f\n", "elapsed_time:", secs);
	printf("  %-17s { seen: %"PRIu64", reported: %"PRIu64", skipped: %"
	       PRIu64" }\n", "objects:", seen, emitted, skipped);
	printf("  %-17s { files: %"PRIu64", dirs: %"PRIu64", dir_shards: %"
	       PRIu64", symlinks: %"PRIu64", other: %"PRIu64", unnamed: %"
	       PRIu64" }\n", "types:", all.files, all.dirs_seen, all.shards,
	       all.symlinks, all.special, all.no_name);
	/* where file_size and file_allocated came from */
	printf("  %-17s { exact: %"PRIu64", lazy_som: %"PRIu64", unknown: %"
	       PRIu64" }\n", "size_source:", all.size_strict, all.size_lazy,
	       all.size_none);
	if (have_ost) {
		printf("  %-17s { ost_objects: %"PRIu64", lazy_som: %"PRIu64
		       ", dom_estimated: %"PRIu64" }\n", "alloc_source:",
		       all.cap_ost, all.cap_lazy, all.dom_plus_ost);
		printf("  %-17s { count: %"PRIu64", bytes: %"PRIu64
		       ", unowned: %"PRIu64", unmatched: %"PRIu64
		       ", unmatched_bytes: %"PRIu64" }\n", "ost_objects:",
		       all.ost_objs, all.ost_blocks * 512, all.ost_unowned,
		       unmatched_objs, unmatched_blocks * 512);
	}
	for (i = 0; i < Y_NR; i++)
		yh_print(stdout, &all.y[i], i);
	if (top > 0) {
		print_top(stdout, &all.dirs, top, 0);
		print_top(stdout, &all.dirs, top, 1);
	}

	getrusage(RUSAGE_SELF, &ru);
	fprintf(stderr, "lfu-stats: %d device(s), %"PRIu64" objects, %.2f s "
		"(%.0f objects/s), max RSS %ld MB\n", argc - optind + nost,
		seen, secs, secs > 0 ? seen / secs : 0, ru.ru_maxrss / 1024);
	return 0;
}
