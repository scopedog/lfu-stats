#!/bin/bash
# Rescan the scale.sh image with a rebuilt lfu-stats.
# usage: rescale.sh TREE WORKDIR TAG
set -eu
T=${1:?tree}
W=${2:?workdir}
TAG=${3:?tag}
PLUG=/usr/lib64/lustre/scan_osd_ldiskfs.so
L=$T/lustre/utils/.libs
cd "$W"
gcc -O2 -g -Wall -o lfu-stats lfu-stats.c -I"$T/include" -I"$T/include/uapi" \
	-I"$T/lustre/include" -L"$L" -llustreapi -lpthread -lm
sudo mount --bind "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
trap 'sudo umount $PLUG' EXIT
cmp "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
for th in 1 4 1 4; do
	sudo env LD_LIBRARY_PATH=$L ./lfu-stats -t $th -n 5 --now 1794700000 \
		-c scale-$TAG-t$th.csv /tmp/lfsb-mdt1 2>&1 > scale-$TAG-t$th.yaml |
		sed "s/^/threads $th: /"
done
cmp scale-$TAG-t1.csv scale-$TAG-t4.csv &&
	echo "csv identical, 1 and 4 threads"
