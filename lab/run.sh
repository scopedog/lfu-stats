#!/bin/bash
# Lab check of lfu-stats against fsstats on the same filesystem.
#   1. format + mount a 2-MDT test fs (FSNAME=lfss), fill it
#   2. fsstats over the client mount: the oracle
#   3. unmount everything, lfu-stats over the MDT images, with and without
#      the two OSTs (rescan.sh)
# usage: run.sh TREE WORKDIR   (TREE: a built Lustre tree with the scan API)
set -eu
T=${1:?tree}
W=${2:?workdir}
TESTS=/usr/lib64/lustre/tests
PLUG=/usr/lib64/lustre/scan_osd_ldiskfs.so
export FSNAME=lfss MDSCOUNT=2 OSTCOUNT=2 MDSSIZE=400000 OSTSIZE=400000

cd "$W"
gcc -O2 -g -Wall -o lfu-stats lfu-stats.c -I"$T/include" \
	-I"$T/include/uapi" -I"$T/lustre/include" -L"$T/lustre/utils/.libs" -llustreapi -lpthread -lm
export LD_LIBRARY_PATH=$T/lustre/utils/.libs
LD_TRACE_LOADED_OBJECTS=1 ./lfu-stats | grep lustreapi

echo "== mount $(date)"
sudo -E bash $TESTS/llmount.sh > llmount.log 2>&1
mount | grep lfss
sudo bash ./populate.sh /mnt/lfss
lfs df -i /mnt/lfss | tail -4

echo "== fsstats $(date)"
date +%s > now
(cd /mnt/lfss && perl "$W/fsstats" -o "$W/fsstats.csv" . > "$W/fsstats.txt" 2>&1)

echo "== unmount $(date)"
sudo -E bash $TESTS/llmountcleanup.sh > cleanup.log 2>&1
mount | grep lfss || true

echo "== lfu-stats $(date)"
# prove the scan opens the tree's plugin, not a stale installed one
sudo mount --bind "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
sudo env LD_LIBRARY_PATH=$LD_LIBRARY_PATH strace -f -e trace=openat \
	-o strace.out ./lfu-stats -t 1 -n 3 /tmp/lfss-mdt1 > /dev/null
grep scan_osd strace.out
sudo umount $PLUG
bash "$W/rescan.sh" "$T" "$W"
for i in 1 2; do
	sudo env LD_LIBRARY_PATH=$LD_LIBRARY_PATH ./lfu-stats --now "$(cat now)" \
		-c lfu-mdt$i.csv /tmp/lfss-mdt$i > lfu-mdt$i.txt
done
sed -n 1,8p lfu.txt
