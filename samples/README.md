# Sample output

From the lab run of 2026-11-14 (see `../lab/`). All lfu-stats files come
from the committed `lfu-stats.c`.

**DNE fixture.** A 2-MDT test filesystem filled by `lab/populate.sh`: about
5,600 objects, including a remote directory, a striped directory, hard
links, symlinks, special files and DoM.

| file | what it is |
|---|---|
| `dne-fixture.txt` | lfu-stats report over both MDT images: summary, histograms, largest directories |
| `dne-fixture.csv` | the same histograms, written with `-c` in fsstats' CSV format |
| `dne-fixture-fsstats.txt` | fsstats 1.4.5 run over the client mount of the same filesystem |
| `dne-fixture-fsstats.csv` | fsstats' CSV for that run |
| `dne-fixture-csv.diff` | `diff dne-fixture-fsstats.csv dne-fixture.csv` |

The diff shows only these known differences:
- capacity used and over/under-allocation: lazy SOM blocks
- filename length: off by one, because fsstats also counts its start path
- the relative/absolute symlink split: not available from a scan
- the time averages: seconds between the two runs

**Scale.** `scale-100k-dirs.txt` is a 1-MDT image made by `lab/scale.sh`:
100,101 directories and 500,000 files, scanned with 4 threads.
