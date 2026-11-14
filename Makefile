# LUSTRE: a built lustre-release tree that has llapi_scan_device() (LU-20611)
# and named device scans (LU-20637), or leave it empty to use the installed
# headers and library.
LUSTRE ?=
CFLAGS ?= -O2 -g -Wall
ifneq ($(LUSTRE),)
CPPFLAGS += -I$(LUSTRE)/include -I$(LUSTRE)/include/uapi -I$(LUSTRE)/lustre/include
LDFLAGS += -L$(LUSTRE)/lustre/utils/.libs
endif
LDLIBS = -llustreapi -lpthread -lm

lfu-stats: lfu-stats.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f lfu-stats

.PHONY: clean
