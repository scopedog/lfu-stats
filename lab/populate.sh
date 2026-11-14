#!/bin/bash
# Fill a test filesystem with a mix the histograms can tell apart.
# usage: populate.sh MOUNT   (DNE: needs MDSCOUNT >= 2)
set -eu
M=${1:?mount point}
CM=/usr/lib64/lustre/tests/createmany

cd "$M"
mkdir -p a/b/c a/b2 docs old
# sizes across many log2 buckets
for s in 0 1 3 9 27 81 243 729 2187; do
	dd if=/dev/urandom of=a/f_${s}k bs=1k count=$s status=none
done
dd if=/dev/urandom of=a/b/c/big bs=1M count=6 status=none
truncate -s 10M a/b/sparse			# size >> blocks
for i in $(seq 1 40); do echo "line $i" > a/b2/small_$i; done

# hard links, inside one directory and across directories
ln a/f_27k a/f_27k.hl
ln a/f_81k a/b/f_81k.hl
ln a/f_81k docs/f_81k.hl2

# symlinks, relative and absolute, of different lengths
ln -s f_1k a/s_rel
ln -s ../../a/f_3k a/b/c/s_rel2
ln -s "$M/a/b/c/big" docs/s_abs
ln -s "$(printf 'x%.0s' $(seq 1 150))" docs/s_long

# special files
mkfifo docs/fifo1 docs/fifo2

# long names
touch "docs/$(printf 'n%.0s' $(seq 1 100))"
touch "docs/$(printf 'm%.0s' $(seq 1 200))"

# one big directory
mkdir big
$CM -o big/f 3000 >/dev/null

# old timestamps
for d in 1 3 10 40 400 2000; do
	echo "$d days" > old/age_$d
	touch -m -d "$d days ago" old/age_$d
	touch -a -d "$((d / 2)) days ago" old/age_$d
done

# DoM: data on the MDT, size from the inode
lfs setstripe -E 1M -L mdt -E -1 docs/dom
dd if=/dev/urandom of=docs/dom bs=1k count=100 status=none
# DoM and OST data in one file: lfu-stats counts it, without its MDT part
lfs setstripe -E 64K -L mdt -E -1 -c 1 docs/dom_ost
dd if=/dev/urandom of=docs/dom_ost bs=1k count=1024 status=none

# DNE: a remote directory and a striped one
lfs mkdir -i 1 remote
$CM -o remote/r 500 >/dev/null
mkdir remote/sub
echo x > remote/sub/leaf
lfs mkdir -c 2 striped
$CM -o striped/s 2000 >/dev/null
mkdir striped/inner
echo y > striped/inner/z
ln a/f_9k striped/f_9k.hl	# a hard link into the striped directory

sync
