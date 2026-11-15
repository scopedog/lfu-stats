#!/bin/bash
# Cost of the OST pass's owner table: N files that each have OST data.
# usage: scale-ost.sh TREE WORKDIR NFILES_K   (thousands of files)
set -eu
T=${1:?tree}
W=${2:?workdir}
K=${3:-100}
TESTS=/usr/lib64/lustre/tests
PLUG=/usr/lib64/lustre/scan_osd_ldiskfs.so
L=$T/lustre/utils/.libs
export FSNAME=lfsc MDSCOUNT=1 OSTCOUNT=1 MDSSIZE=2000000 OSTSIZE=4000000

cd "$W"
echo "== mount $(date)"
sudo -E bash $TESTS/llmount.sh > scale-ost-llmount.log 2>&1
# K directories of 1000 files, 1 byte each: an owned OST object per file
sudo python3 -c '
import os, sys
m, k = sys.argv[1], int(sys.argv[2])
for d in range(k):
    os.mkdir("%s/d%d" % (m, d))
    for f in range(1000):
        with open("%s/d%d/f%d" % (m, d, f), "w") as fh:
            fh.write("x")
' /mnt/lfsc "$K"
# llmountcleanup.sh unmounts the client with -f, which drops dirty pages
sync
lfs df -i /mnt/lfsc | grep -E "MDT|OST"
echo "== unmount $(date)"
sudo -E bash $TESTS/llmountcleanup.sh > scale-ost-cleanup.log 2>&1

sudo mount --bind "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
trap 'sudo umount $PLUG' EXIT
cmp "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
for r in 1 2; do
	for th in 1 4; do
		echo "== round $r, threads $th: MDT only, then with the OST"
		sudo env LD_LIBRARY_PATH=$L ./lfu-stats -t $th -n 0 \
			/tmp/lfsc-mdt1 > scale-ost-mdt-t$th.yaml
		sudo env LD_LIBRARY_PATH=$L ./lfu-stats -t $th -n 0 \
			--ost /tmp/lfsc-ost1 /tmp/lfsc-mdt1 > scale-ost-t$th.yaml
		grep -E "alloc_source|ost_objects" scale-ost-t$th.yaml
	done
done
