# Lab scripts

Run on a test node with Lustre installed and a built lustre-release tree that
has the LFU device scanner. Copy `lfu-stats.c`, these scripts and `fsstats`
(from https://github.com/adilger/fsstats) into one work directory.

- `run.sh TREE WORKDIR` formats a 2-MDT, 2-OST test filesystem (FSNAME=lfss),
  fills it with `populate.sh`, runs fsstats over the client mount as the
  oracle, unmounts it, and runs lfu-stats over both MDT images. Leaves
  `csv.diff`.
- `rescan.sh TREE WORKDIR` rebuilds lfu-stats and rescans the same images.
- `scale.sh TREE WORKDIR K` builds a 1-MDT filesystem with K x 1000
  directories and K x 5000 files and measures time and memory.
- `rescale.sh TREE WORKDIR TAG` rescans that image.
- `null_scan.c` requests the same fields and does nothing, for the timing
  comparison.

The scripts bind-mount the tree's `scan_osd_ldiskfs.so` over the installed
one for the run: the scan library loads the installed plugin first.

Expected differences from fsstats, on the 2026-11-14 run: capacity used and
over/underhead (lazy SOM blocks), filename length (fsstats counts its start
path `.`), the relative/absolute symlink split (not available), and the time
averages (seconds between the two runs).
