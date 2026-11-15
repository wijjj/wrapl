PREFIX      ?= /usr/local
SBINDIR     ?= $(PREFIX)/sbin
UNITDIR     ?= /etc/systemd/system
SYSUSERSDIR ?= /etc/sysusers.d

CFLAGS  ?= -O2
# the project's own flags, kept when CFLAGS or LDFLAGS is set on the command line: warnings, then hardening
WFLAGS   = -std=c11 -Wall -Wextra -Wpedantic
HCFLAGS  = -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE
HLDFLAGS = -pie -Wl,-z,relro,-z,now

all: wrapl wrapl.service

wrapl: wrapl.c
	$(CC) $(WFLAGS) $(HCFLAGS) $(CPPFLAGS) $(CFLAGS) $(HLDFLAGS) $(LDFLAGS) -o $@ $<

# checked on every run, and replaced when SBINDIR changed since it was made, so the unit always starts the binary
# that install puts in place
wrapl.service: wrapl.service.in FORCE
	sed 's|@SBINDIR@|$(SBINDIR)|g' wrapl.service.in > $@.tmp
	if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

install: all
	install -Dm755 wrapl $(DESTDIR)$(SBINDIR)/wrapl
	install -Dm755 wrapl-perms $(DESTDIR)$(SBINDIR)/wrapl-perms
	install -Dm644 wrapl.service $(DESTDIR)$(UNITDIR)/wrapl.service
	install -Dm644 wrapl.sysusers $(DESTDIR)$(SYSUSERSDIR)/wrapl.conf
	# create the wrapl user now when installing onto this system; a package does it at its own install time
	if [ -z "$(DESTDIR)" ]; then systemd-sysusers $(SYSUSERSDIR)/wrapl.conf; fi

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/wrapl $(DESTDIR)$(SBINDIR)/wrapl-perms $(DESTDIR)$(UNITDIR)/wrapl.service $(DESTDIR)$(SYSUSERSDIR)/wrapl.conf

test: wrapl
	./test.sh

clean:
	rm -f wrapl wrapl.service wrapl.service.tmp

FORCE:

.PHONY: all install uninstall test clean FORCE
