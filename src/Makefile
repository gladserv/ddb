# Makefile for the ddb program
#
# Copyright (c) 2018 Claudio Calvelli <ddb@gladserv.com>
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. If the program is modified in any way, a line must be added to the
#    above copyright notice to state that such modification has occurred.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS \"AS IS\" AND
# ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.

.PHONY:	all install clean realclean

CFLAGS += -I$(ROOTDIR)/include -I$(ROOTDIR)/lib -g
LDFLAGS += -L$(ROOTDIR)/lib

all:	ddb ddb-info ddb-sequence ddb-daemon

ddb:	ddb.o $(ROOTDIR)/lib/libddb.so
	$(CC) $(LDFLAGS) -o ddb ddb.o -lddb

ddb.o:	ddb.c ../include/ddb.h

ddb-info: ddb-info.o $(ROOTDIR)/lib/libddb.so
	$(CC) $(LDFLAGS) -o ddb-info ddb-info.o -lddb

ddb-info.o: ddb-info.c ../include/ddb.h

ddb-sequence: ddb-sequence.o $(ROOTDIR)/lib/libddb.so
	$(CC) $(LDFLAGS) -o ddb-sequence ddb-sequence.o -lddb

ddb-sequence.o: ddb-sequence.c ../include/ddb.h

ddb-daemon: ddb-daemon.o $(ROOTDIR)/lib/libddb.so
	$(CC) $(LDFLAGS) -o ddb-daemon ddb-daemon.o -lddb

ddb-daemon.o: ddb-daemon.c ../include/ddb.h

install: all
	install -d $(BIN_PATH)
	install ddb ddb-info ddb-sequence ddb-daemon $(BIN_PATH)/

clean:
	rm -f *.o

realclean:
	rm -f *.o ddb ddb-info ddb-sequence ddb-daemon

