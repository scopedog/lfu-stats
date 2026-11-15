# lfu-stats

Filesystem statistics from a Lustre MDT scan: size, space, age and
directory histograms, plus the largest directories, as YAML.

`lfu-stats` reads one or more MDT devices, and optionally the OSTs, with
`llapi_scan_device()` (LFU, LU-20462), without walking the namespace from a
client. It covers the attributes and histograms of
[fsstats](https://github.com/adilger/fsstats), which served as the public
reference, and adds the largest N directories. This is a prototype; the plan
is to build it into `lfs find` as `--report`.

```
lfu-stats [-t THREADS] [-n TOP] [-c CSVFILE] [--now EPOCH] [--ost OSTDEV]...
          MDTDEV...
```

- `MDTDEV` is an MDT that is not mounted (or a snapshot of one). Give every
  MDT of a DNE filesystem to get one merged report.
- `--ost OSTDEV` (repeatable) scans that OST first. Files then take their
  capacity used from their OST objects instead of the MDT's lazy SOM blocks.
  Give every OST for exact figures. A device given as the wrong kind of
  target is refused.
- `-c` also writes fsstats' histograms in its CSV format, so the result can
  be diffed against fsstats run on the same filesystem (the lab check).
- `--now` sets the reference time for the age histograms.

## Output

The report goes to standard output as YAML in the style of `job_stats`: one
line per statistic, readable as it is and parseable by any YAML library. The
run summary (objects/s, memory) goes to standard error. From
`samples/dne-fixture.yaml`, shortened:

```yaml
scan_stats:
- mdts:             [ "/tmp/lfss-mdt1", "/tmp/lfss-mdt2" ]
  osts:             [ "/tmp/lfss-ost1", "/tmp/lfss-ost2" ]
  snapshot_time:    1794691965
  elapsed_time:     0.013
  objects:          { seen: 12386, reported: 11284, skipped: 0 }
  types:            { files: 5563, dirs: 12, dir_shards: 2, symlinks: 4, other: 2, unnamed: 0 }
  size_source:      { exact: 1, lazy_som: 5562, unknown: 0 }
  alloc_source:     { ost_objects: 61, lazy_som: 5501, dom_estimated: 1 }
  file_size:        { samples: 5563, unit: bytes, min: 0, max: 10485760, sum: 21287276, hist: { 0: 5503, 2: 2, 4: 11, 8: 35, 1K: 1, ... }, hist_bytes: { ... } }
  dir_entries:      { samples: 12, unit: entries, min: 1, max: 3000, sum: 5584, hist: { 1: 2, 2: 2, 4: 2, 8: 2, 32: 1, 256: 1, 1K: 1, 2K: 1 } }
  mtime_age:        { samples: 5563, unit: secs, min: 0, max: 172800001, sum: 212031461, hist: { 0: 5557, 1d: 2, 1w: 1, 1M: 1, 1y: 1, 5y: 1 }, hist_bytes: { ... } }
  largest_dirs_by_entries:
  - { entries: 3000, file_bytes: 0, dir_size: 135168, path: "/big" }
  - { entries: 2002, file_bytes: 0, dir_size: 98304, path: "/striped" }
```

- `hist` keys are each bucket's lower bound: powers of two (`1K` = 1024) as
  in `job_stats`, or ages `1h 1d 1w 1M 3M 6M 1y 2y 5y 10y` (`future` for a
  time ahead of `snapshot_time`). Byte-weighted statistics add `hist_bytes`,
  the bytes in each bucket.
- The statistics: `file_size`, `file_allocated`, `file_overhead` (allocated
  beyond size), `file_sparse` (size beyond allocated), `dir_entries`,
  `dir_size`, `name_length` (every name, hard links included), `file_links`,
  `symlink_length`, and `mtime_age`, `ctime_age`, `atime_age` (regular files).
- `size_source` and `alloc_source` count where each file's numbers came
  from, so a reader can tell exact figures from lazy ones.

## Where each number comes from

| fsstats histogram (YAML key) | MDT scan source | Accuracy |
|---|---|---|
| file size | `trusted.som`, or the inode for DoM | matched fsstats exactly in the lab |
| capacity used, over/underhead | with `--ost`: the blocks of every OST object owned by the file (`trusted.fid`), as a client's `stat` sums them | matched exactly |
| | without `--ost`: `trusted.som` blocks | **lazy**: low for files whose blocks were allocated after close |
| directory size (entries) | count of linkea entries naming each directory | matched exactly, striped directories included |
| directory size (KB) | directory inode size, shards added to the master | matched exactly |
| filename length | every linkea name | matched (fsstats also counts its start path) |
| link count | `nlink` | matched |
| symlink target length | symlink inode size | matched; relative/absolute split is not available |
| m/c/atime age | MDT inode times | matched; MDT atime is lazy in general |

Entries per directory are not stored in the directory inode. Each object's
linkea names its parent(s), so the scan counts names per parent FID. Children
of a striped directory name the shard as parent; the shard's own linkea
names the master, and the counts are folded into it after the scan. Paths
for the top directories are built the same way, from each directory's first
linkea entry. A directory whose ancestors are on an MDT that was not scanned
is printed relative to the FID where the walk stopped.

With `--ost`, a file with both DoM and OST data gets its MDT part estimated
from the end of its DoM component. Once a file has data on the OSTs, that
component has been written in full, so the estimate is exact unless the
component has holes. The report counts these files. The report also counts
OST objects that no scanned MDT file owns: orphans, or files on an MDT that
was not given.

In the lab (2 MDTs, 2 OSTs, including a DoM+OST file), the fsstats CSV and
the lfu-stats CSV with `--ost` differ only in four places: filename length
(fsstats also counts its start path), the relative/absolute symlink split
(not available), the age averages (seconds between the two runs), and the
header.

## Cost

The per-directory table is the only state that grows with the filesystem:
one entry per directory (72 bytes, kept at most half full) plus its name.

Measured on a 4-vCPU VM, 1 MDT (ldiskfs) image, 100,101 directories and
500,000 files, warm cache, 3 alternating runs each:

| | 1 thread | 4 threads |
|---|---|---|
| scan with a consumer that does nothing, same fields | 0.30 s | 0.10-0.11 s |
| lfu-stats | 0.38 s (1.6M objects/s) | 0.14-0.15 s (4.0-4.2M objects/s) |
| max RSS, lfu-stats | 32 MB | 42-46 MB (varies with how the work spreads over threads) |
| max RSS, a scan that keeps no state | 3.5 MB | 3.6 MB |

So the directory table costs about 220 bytes per directory steady-state and
about 300 bytes while the table doubles: roughly 11-15 GB for 50M
directories. Two passes would cut that: a first pass keeping only
FID -> counts, and a second collecting names only for the ancestors of the
top N.

The CSV output is identical with 1 and 4 threads.

With `--ost`, the owner table holds one entry per file that has OST data
(32 bytes, kept at most half full). On a 1-MDT, 1-OST image with 100,000
files each holding data: max RSS 15-17 MB with `--ost`, 3 MB without, so
about 120-140 bytes per file just after the table doubled (64-128 steady).
The OST scan adds about 0.05 s per 100k objects. At a billion files that
would be 64-128 GB; at that scale the OST pass would have to run in FID-hash
ranges (one OST scan per range) or spill sorted (FID, blocks) pairs to disk.

## Limits

- Without `--ost`, capacity used comes from lazy SOM.
- Linkea can be truncated for files with very many hard links; those names
  are not counted.
- A hard-linked file's bytes are counted in its first parent only.
- The scan reads the device directly, so the MDT must be unmounted or a
  snapshot.

## Building

Needs a lustre-release tree with the LFU device scanner (LU-20611, LU-20637):

```
make LUSTRE=/path/to/lustre-release
```

`lab/` has the scripts used to check it against fsstats on a test filesystem.
