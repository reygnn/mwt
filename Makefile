PREFIX ?= /usr/local
CC ?= cc
PKGS = wayland-client wayland-cursor xkbcommon fontconfig freetype2
WP != pkg-config --variable=pkgdatadir wayland-protocols
SCANNER != pkg-config --variable=wayland_scanner wayland-scanner

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) -I.
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lutil -lm

PROTOS = xdg-shell xdg-decoration-unstable-v1 primary-selection-unstable-v1 viewporter
xdg-shell_XML = $(WP)/stable/xdg-shell/xdg-shell.xml
viewporter_XML = $(WP)/stable/viewporter/viewporter.xml
xdg-decoration-unstable-v1_XML = $(WP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
primary-selection-unstable-v1_XML = $(WP)/unstable/primary-selection/primary-selection-unstable-v1.xml

PROTO_H = $(PROTOS:=-client-protocol.h)
PROTO_C = $(PROTOS:=-protocol.c)
OBJ = mwt.o vt.o font.o rc.o $(PROTO_C:.c=.o)

all: mwt

mwt: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

# mwt that answers DECRQCRA, which esctest reads the screen with; see allow_checksum.
mwt-esctest: mwt.c config.h version.h vt.h font.h rc.h $(PROTO_H) $(filter-out mwt.o,$(OBJ))
	$(CC) $(CFLAGS) -DMWT_ALLOW_CHECKSUM $(LDFLAGS) -o $@ mwt.c $(filter-out mwt.o,$(OBJ)) $(LDLIBS)

mwt.o: mwt.c config.h version.h vt.h font.h rc.h $(PROTO_H)
vt.o: vt.c vt.h version.h
font.o: font.c font.h
rc.o: rc.c rc.h

%-client-protocol.h:
	$(SCANNER) client-header $($*_XML) $@

%-protocol.c:
	$(SCANNER) private-code $($*_XML) $@

test_vt: test_vt.c vt.c vt.h version.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc -o $@ test_vt.c vt.c

test_rc: test_rc.c rc.c rc.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ test_rc.c rc.c -lm

# test_vt and test_rc, then every input kept in tests/fuzz-regress through the checks of
# tests/fuzz_vt.c; no clang needed for that.
check: test_vt test_rc fuzz_regress
	./test_vt
	./test_rc
	@./fuzz_regress tests/fuzz-regress/* 2>fuzz_regress.log || { cat fuzz_regress.log; exit 1; }
	@rm -f fuzz_regress.log

# font.c with each strdup and FcPatternDuplicate failing once in turn, under
# sanitizers; needs fallback fonts to mean anything, so make fonts runs it
# (tests/fonts-run.sh).
font_oom: tests/font_oom.c font.c font.h
	$(CC) -std=c11 -Wall -Wextra -g -O1 -I. -fsanitize=address,undefined \
		-fno-sanitize-recover=all $(shell pkg-config --cflags fontconfig freetype2) \
		-Wl,--wrap=strdup -Wl,--wrap=FcPatternDuplicate -o $@ tests/font_oom.c font.c \
		$(shell pkg-config --libs fontconfig freetype2) -lm

# The glyph cache emptied at small limits, font.c built into the test; under
# sanitizers, with a font that has Latin glyphs. make fonts runs it.
font_cache: tests/font_cache.c font.c font.h
	$(CC) -std=c11 -Wall -Wextra -g -O1 -I. -fsanitize=address,undefined \
		-fno-sanitize-recover=all $(shell pkg-config --cflags fontconfig freetype2) \
		-o $@ tests/font_cache.c $(shell pkg-config --libs fontconfig freetype2) -lm

# Glyphs no open face has: one FcFontSort for all, counted through --wrap; under
# sanitizers, with the fonts of make fonts.
font_lookup: tests/font_lookup.c font.c font.h
	$(CC) -std=c11 -Wall -Wextra -g -O1 -I. -fsanitize=address,undefined \
		-fno-sanitize-recover=all $(shell pkg-config --cflags fontconfig freetype2) \
		-Wl,--wrap=FcFontMatch -Wl,--wrap=FcFontSort -o $@ tests/font_lookup.c \
		$(shell pkg-config --libs fontconfig freetype2) -lm

# Each style draws what regular has, where its own face lacks it; under
# sanitizers, with DejaVu Sans Mono. make fonts runs it.
font_style: tests/font_style.c font.c font.h
	$(CC) -std=c11 -Wall -Wextra -g -O1 -I. -fsanitize=address,undefined \
		-fno-sanitize-recover=all $(shell pkg-config --cflags fontconfig freetype2) \
		-o $@ tests/font_style.c $(shell pkg-config --libs fontconfig freetype2) -lm

fuzz_regress: tests/fuzz_vt.c vt.c vt.h version.h
	$(CC) -std=c11 -Wall -Wextra -g -O1 -I. -DFUZZ_STANDALONE -fsanitize=address,undefined \
		-fno-sanitize-recover=all -o $@ tests/fuzz_vt.c vt.c

test_replay: tests/replay.c vt.c vt.h version.h
	$(CC) -std=c11 -Wall -Wextra -g -O1 -I. -fsanitize=address,undefined \
		-fno-sanitize-recover=all -o $@ tests/replay.c vt.c

# About 290 recorded VT100 animations and ANSI files through the VT core, whole,
# byte by byte, in random chunks and while resizing; fetched on the first run.
# REPORT=1 lists what each file uses that mwt does not support.
replay: test_replay
	tests/replay.sh $(if $(REPORT),-r)

# Coverage-guided fuzzing of the VT core with libFuzzer, see tests/fuzz_vt.c.
# Ten minutes by default (FUZZ_TIME=seconds), on all cores. Uses clang on the
# host if it has libFuzzer, otherwise the container of make evil (tests/fuzz.sh).
# The corpus grows in tests/fuzz/corpus, a finding is tests/fuzz/crash-*.
FUZZ_TIME ?= 600
fuzz_vt: tests/fuzz_vt.c vt.c vt.h version.h
	clang -std=c11 -Wall -Wextra -g -O1 -I. -fsanitize=fuzzer,address,undefined \
		-fno-sanitize-recover=all -o $@ tests/fuzz_vt.c vt.c

fuzz:
	FUZZ_TIME=$(FUZZ_TIME) tests/fuzz.sh

# Each capability of mwt.info against the VT core. Needs python3 and ncurses.
vtdump: tests/vtdump.c vt.c vt.h version.h
	$(CC) -std=c11 -Wall -Wextra -g -fsanitize=address,undefined -I. -o $@ tests/vtdump.c vt.c

terminfo: vtdump
	tests/terminfo.py

# What mwt sends for keys and the mouse, and what it draws, under a headless sway.
keys: mwt
	tests/headless.sh tests/keys.py

pixel: mwt
	tests/headless.sh tests/pixel.py

mouse: mwt
	tests/headless.sh tests/mouse.py

keyboard: mwt
	tests/headless.sh tests/keyboard.py

# Runs mwt under a headless sway with wtype, wl-clipboard, grim and mwc's vinput.
test: mwt
	tests/run.sh

# The tests and the adversarial cases in a Podman container, with sanitizers too;
# needs no compositor or tools on the host, and takes about 20 minutes.
evil:
	tests/container.sh

# Glyphs from fallback faces (CJK, emoji, many scripts) in an image with more
# fonts than the one of `make evil`; see tests/Containerfile.fonts.
fonts:
	tests/fonts.sh

# The cases that flood the compositor with requests, against libwayland 1.22,
# which ends the connection where newer ones grow their buffer; Ubuntu 24.04 in
# Podman (tests/Containerfile.2404).
oldwayland:
	tests/oldwayland.sh

# Where mwt and bash, vim or tmux count a character's width differently.
# Needs what `make test` needs plus vim and tmux.
width: mwt
	tests/width.sh

# esctest under a headless sway, a report (see tests/esctest.sh); fetched on the
# first run. Needs sway and python3.
esctest: mwt-esctest
	tests/esctest.sh

# Where mwt spends its time, under a headless sway in the container of `make evil`.
# Measures only, about 3 minutes. PROFILE_WORK picks loads (see tests/profile-run.sh).
profile:
	tests/profile.sh

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
	rm -f mwt mwt-esctest test_vt test_rc vtdump test_replay fuzz_vt fuzz_regress fuzz_regress.log font_oom font_cache font_style font_lookup *.o $(PROTO_H) $(PROTO_C)

.PHONY: all check replay fuzz terminfo keys pixel mouse keyboard test evil fonts oldwayland width esctest profile install uninstall clean
