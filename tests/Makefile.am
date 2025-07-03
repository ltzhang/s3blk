# SPDX-License-Identifier: MIT or GPL-2.0-only

# XXX Fix to use autotools TESTS = ...

.PHONY: run clean

# Test programs
noinst_PROGRAMS = pageserver_test_client

pageserver_test_client_SOURCES = pageserver_test_client.cpp
pageserver_test_client_CFLAGS = $(WARNINGS_CFLAGS) $(LIBURING_CFLAGS) $(PTHREAD_CFLAGS)
pageserver_test_client_CPPFLAGS = $(pageserver_test_client_CFLAGS) -I$(top_srcdir)/include -I$(top_srcdir)/targets/pageserver
pageserver_test_client_LDADD = $(LIBURING_LIBS) $(PTHREAD_LIBS)

run:
	$(srcdir)/run_test.sh ${T} ${R} ${D}

CLEANFILES = *~ */*~

EXTRA_DIST = \
	common/fio_common \
	common/loop_common \
	generic/001 \
	generic/002 \
	generic/003 \
	loop/001 \
	loop/002 \
	loop/003 \
	loop/004 \
	loop/005 \
	loop/006 \
	loop/007 \
	null/001 \
	null/002 \
	null/004 \
	null/005 \
	null/006 \
	run_test.sh
