#!/bin/bash
# Cost of the per-directory table: a 1-MDT fs with many directories.
# usage: scale.sh TREE WORKDIR NDIRS_K   (thousands of directories)
set -eu
T=${1:?tree}
W=${2:?workdir}
K=${3:-100}
TESTS=/usr/lib64/lustre/tests
CM=$TESTS/createmany
PLUG=/usr/lib64/lustre/scan_osd_ldiskfs.so
L=$T/lustre/utils/.libs
export FSNAME=lfsb MDSCOUNT=1 OSTCOUNT=1 MDSSIZE=6000000 OSTSIZE=400000

cd "$W"
echo "== mount $(date)"
sudo -E bash $TESTS/llmount.sh > scale-llmount.log 2>&1
M=/mnt/lfsb
# K top directories of 1000 subdirectories, and 500 files in 1 in 10 of them
for i in $(seq 1 "$K"); do
	sudo mkdir $M/t$i
	sudo $CM -d $M/t$i/d 1000 > /dev/null
	for j in $(seq 0 100 999); do
		sudo $CM -m $M/t$i/d$j/f 500 > /dev/null
	done
done
sync	# llmountcleanup.sh unmounts the client with -f
lfs df -i $M | grep MDT
echo "== unmount $(date)"
sudo -E bash $TESTS/llmountcleanup.sh > scale-cleanup.log 2>&1

sudo mount --bind "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
trap 'sudo umount $PLUG' EXIT
cmp "$T/lustre/utils/scan_osd_ldiskfs.so" $PLUG
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
for th in 4 1 4; do
	echo "== threads $th"
	sudo env LD_LIBRARY_PATH=$L ./lfu-stats -t $th -n 5 \
		-c scale-t$th.csv /tmp/lfsb-mdt1 > scale-t$th.txt
	head -3 scale-t$th.txt
done
cmp scale-t1.csv scale-t4.csv && echo "csv: 1 and 4 threads identical"
sed -n '/^largest 5 directories by entries/,/^$/p' scale-t4.txt
