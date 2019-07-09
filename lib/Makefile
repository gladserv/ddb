# Makefile for the ddb library
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

OBJECTS_NORMAL = ddb-blocklist.o ddb-device.o ddb-config.o ddb-image.o ddb-dir.o \
	  ddb-checksum.o ddb-lvm.o ddb-util.o ddb-copy.o ddb-error.o
OBJECTS_REMOTE = ddb-plugin.o ddb-remote.o
OBJECTS = $(OBJECTS_NORMAL) $(OBJECTS_REMOTE)
CFLAGS += -fPIC -I $(ROOTDIR)/include -g -DDEFAULT_MODPATH=\"$(DEFAULT_MODPATH)\"
LDFLAGS += -shared -fPIC

.PHONY:	all install clean realclean

all:	libddb.so

libddb.so: $(OBJECTS)
	$(CC) $(LDFLAGS) -o libddb.so $(OBJECTS) -ldl -lcrypto

$(OBJECTS_NORMAL):: ddb-private.h ../include/ddb.h

$(OBJECTS_REMOTE):: ddb-private.h ddb-remote.h ../include/ddb.h

install: all
	install -d $(LIB_PATH)
	install libddb.so $(LIB_PATH)/
	ldconfig

clean:
	rm -f $(OBJECTS)

realclean:
	rm -f $(OBJECTS) libddb.so

