/* The scan lfu-stats asks for, with a consumer that does nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <lustre/lustreapi.h>

static int null_cb(const struct llapi_scan_rec *rec, void *data)
{
	return 0;
}

int main(int argc, char **argv)
{
	struct llapi_scan_param sp = {
		.lfsp_size = sizeof(sp),
		.lfsp_want = LLAPI_SCAN_TYPE | LLAPI_SCAN_MODE |
			     LLAPI_SCAN_NLINK | LLAPI_SCAN_SIZE |
			     LLAPI_SCAN_BLOCKS | LLAPI_SCAN_LAZY_SIZE |
			     LLAPI_SCAN_LAZY_BLOCKS | LLAPI_SCAN_ATIME |
			     LLAPI_SCAN_MTIME | LLAPI_SCAN_CTIME |
			     LLAPI_SCAN_FID | LLAPI_SCAN_CLASS |
			     LLAPI_SCAN_LINKEA | LLAPI_SCAN_LMV,
		.lfsp_thread_count = atoi(argv[2]),
	};
	struct timespec a, b;
	int rc;

	clock_gettime(CLOCK_MONOTONIC, &a);
	rc = llapi_scan_device(argv[1], &sp, null_cb, NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("null scan, %s threads: rc %d, %.2f s\n", argv[2], rc,
	       (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
	return rc < 0;
}
