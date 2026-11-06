PREFIX ?= /usr/local
CC ?= cc
PKGS = wayland-client wayland-cursor xkbcommon fontconfig freetype2
WP != pkg-config --variable=pkgdatadir wayland-protocols
SCANNER != pkg-config --variable=wayland_scanner wayland-scanner

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) -I.
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lutil -lm

PROTOS = xdg-shell xdg-decoration-unstable-v1 primary-selection-unstable-v1
xdg-shell_XML = $(WP)/stable/xdg-shell/xdg-shell.xml
xdg-decoration-unstable-v1_XML = $(WP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
primary-selection-unstable-v1_XML = $(WP)/unstable/primary-selection/primary-selection-unstable-v1.xml

PROTO_H = $(PROTOS:=-client-protocol.h)
PROTO_C = $(PROTOS:=-protocol.c)
OBJ = mwt.o vt.o font.o $(PROTO_C:.c=.o)

all: mwt

mwt: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

mwt.o: mwt.c config.h vt.h font.h $(PROTO_H)
vt.o: vt.c vt.h
font.o: font.c font.h

%-client-protocol.h:
	$(SCANNER) client-header $($*_XML) $@

%-protocol.c:
	$(SCANNER) private-code $($*_XML) $@

test_vt: test_vt.c vt.c vt.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc -o $@ test_vt.c vt.c

check: test_vt
	./test_vt

# Each capability of mwt.info against the VT core. Needs python3 and ncurses.
vtdump: tests/vtdump.c vt.c vt.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined -I. -o $@ tests/vtdump.c vt.c

terminfo: vtdump
	tests/terminfo.py

# What mwt sends for keys and the mouse, and what it draws, under a headless sway.
keys: mwt
	tests/headless.sh tests/keys.py

pixel: mwt
	tests/headless.sh tests/pixel.py

# Runs mwt under a headless sway with wtype, wl-clipboard, grim and mwc's vinput.
test: mwt
	tests/run.sh

# The tests and the adversarial cases in a Podman container, with sanitizers too;
# needs no compositor or tools on the host, and takes about 20 minutes.
evil:
	tests/container.sh

# Where mwt and bash, vim or tmux count a character's width differently.
# Needs what `make test` needs plus vim and tmux.
width: mwt
	tests/width.sh

install: mwt
	install -Dm755 mwt $(DESTDIR)$(PREFIX)/bin/mwt
	# Not under PREFIX: ncurses does not search /usr/local/share/terminfo.
	install -d $(DESTDIR)/usr/share/terminfo
	tic -x -o $(DESTDIR)/usr/share/terminfo mwt.info

# Also removes the entry older versions put under PREFIX. tic picks m/ or 6d/.
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mwt
	rm -f $(DESTDIR)/usr/share/terminfo/m/mwt $(DESTDIR)/usr/share/terminfo/6d/mwt
	rm -f $(DESTDIR)$(PREFIX)/share/terminfo/m/mwt $(DESTDIR)$(PREFIX)/share/terminfo/6d/mwt

clean:
	rm -f mwt test_vt vtdump *.o $(PROTO_H) $(PROTO_C)

.PHONY: all check terminfo keys pixel test evil width install uninstall clean
