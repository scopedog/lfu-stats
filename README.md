# lfu-stats

fsstats-style filesystem reports from a Lustre MDT scan, plus the largest
directories.

`lfu-stats` reads one or more MDT devices, and optionally the OSTs, with
`llapi_scan_device()` (LFU, LU-20462) and reports the attributes and histograms of
[fsstats](https://github.com/adilger/fsstats), without walking the namespace
from a client. It also lists the largest N directories, which fsstats does
not have. This is a prototype.

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
- `-c` writes the histograms in fsstats' CSV format, so the output can be
  diffed against fsstats run on the same filesystem.
- `--now` sets the reference time for the age histograms.

## Where each number comes from

| fsstats histogram | MDT scan source | Accuracy |
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
