#!/bin/bash
# Rebuild lfu-stats and rescan the images run.sh left, without touching them.
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
sudo env LD_LIBRARY_PATH=$L ./lfu-stats --now "$(cat now)" -c lfu.csv \
	/tmp/lfss-mdt1 /tmp/lfss-mdt2 > lfu.txt
diff fsstats.csv lfu.csv > csv.diff || true
wc -l < csv.diff
