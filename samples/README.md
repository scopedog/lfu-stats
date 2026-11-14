# Sample output

From the lab runs of 2026-11-14 (see `../lab/`). Every lfu-stats file here
was made by the committed `lfu-stats.c`.

**DNE fixture.** A 2-MDT, 2-OST test filesystem filled by `lab/populate.sh`:
about 5,600 objects, including a remote directory, a striped directory, hard
links, symlinks, special files, a DoM file and a DoM+OST file.

| file | what it is |
|---|---|
| `dne-fixture.txt` | lfu-stats with `--ost`: both OSTs, then both MDTs |
| `dne-fixture.csv` | the same histograms, written with `-c` in fsstats' CSV format |
| `dne-fixture-mdtonly.txt`, `.csv` | lfu-stats over the MDTs only (capacity from lazy SOM) |
| `dne-fixture-fsstats.txt`, `.csv` | fsstats 1.4.5 run over the client mount of the same filesystem |
| `dne-fixture-csv.diff` | `diff dne-fixture-fsstats.csv dne-fixture.csv` |
| `dne-fixture-mdtonly-csv.diff` | the same diff for the MDT-only run |

With `--ost`, the diff shows only these known differences:
- filename length: off by one, because fsstats also counts its start path
- the relative/absolute symlink split: not available from a scan
- the age averages: seconds between the two runs
- the header

Without `--ost`, capacity used and the over/under-allocation histograms also
differ, because lazy SOM blocks miss space allocated after close.

**Scale.**
- `scale-100k-dirs.txt`: a 1-MDT image from `lab/scale.sh`, with 100,101
  directories and 500,000 files, scanned with 4 threads.
- `scale-100k-files-ost.txt`: a 1-MDT, 1-OST image from `lab/scale-ost.sh`,
  with 100,000 files that each hold data, scanned with `--ost` and 4 threads.
