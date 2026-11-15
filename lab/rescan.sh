#!/bin/bash
# Rebuild lfu-stats and rescan the images run.sh left, without touching them:
# MDTs only, then with the OST pass, then the two wrong-target checks.
# usage: rescan.sh TREE WORKDIR
set -eu
T=${1:?tree}
W=${2:?workdir}
PLUG=/usr/lib64/lustre/scan_osd_ldiskfs.so
cd "$W"
gcc -O2 -g -Wall -o lfu-stats lfu-stats.c -I"$T/include" -I"$T/include/uapi" \
	-I"$T/lustre/include" -L"$T/lustre/utils/.libs" -llustreapi -lpthread -lm
L=$T/lustre/utils/.libs
sudo mount --bind "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
trap 'sudo umount $PLUG' EXIT
cmp "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
run() { sudo env LD_LIBRARY_PATH=$L ./lfu-stats --now "$(cat now)" "$@"; }

run -c lfu-mdtonly.csv /tmp/lfss-mdt1 /tmp/lfss-mdt2 > lfu-mdtonly.yaml
run -c lfu.csv --ost /tmp/lfss-ost1 --ost /tmp/lfss-ost2 \
	/tmp/lfss-mdt1 /tmp/lfss-mdt2 > lfu.yaml
diff fsstats.csv lfu-mdtonly.csv > csv-mdtonly.diff || true
diff fsstats.csv lfu.csv > csv.diff || true
echo "diff lines: MDT only $(wc -l < csv-mdtonly.diff), with OSTs $(wc -l < csv.diff)"

echo "-- an OST given as an MDT, and an MDT given as an OST (both must fail):"
run /tmp/lfss-ost1 > /dev/null && echo "UNEXPECTED: accepted"
run --ost /tmp/lfss-mdt1 /tmp/lfss-mdt2 > /dev/null && echo "UNEXPECTED: accepted"
true
