PREFIX  ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
UNITDIR ?= /etc/systemd/system

CFLAGS  ?= -O2
# the project's own flags, kept when CFLAGS is set on the command line
WFLAGS   = -std=c11 -Wall -Wextra -Wpedantic

all: wrapl wrapl.service

wrapl: wrapl.c
	$(CC) $(WFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

wrapl.service: wrapl.service.in
	sed 's|@SBINDIR@|$(SBINDIR)|g' $< > $@

install: all
	install -Dm755 wrapl $(DESTDIR)$(SBINDIR)/wrapl
	install -Dm644 wrapl.service $(DESTDIR)$(UNITDIR)/wrapl.service

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/wrapl $(DESTDIR)$(UNITDIR)/wrapl.service

test: wrapl
	./test.sh

clean:
	rm -f wrapl wrapl.service

.PHONY: all install uninstall test clean
