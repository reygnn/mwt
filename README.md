# mwt – minimal Wayland terminal

Ein kleiner Terminal-Emulator für Wayland, geschrieben in C. mwt ist ein eigenständiger xdg-shell-Client und braucht weder X11 noch Xwayland. Er läuft in mwc und in jedem anderen Compositor.

## Bauen

Abhängigkeiten: wayland-client, wayland-cursor, wayland-protocols, wayland-scanner, xkbcommon, fontconfig, freetype2.

```sh
make            # baut ./mwt
make check      # Tests für den VT-Kern (headless, mit ASan/UBSan), auch ein Fuzzer
make replay     # rund 290 alte VT100-Animationen und ANSI-Dateien durch den VT-Kern (holt sie beim ersten Mal von GitHub)
make terminfo   # jede Fähigkeit aus mwt.info gegen den VT-Kern (braucht python3 und ncurses)
make keys       # was mwt für Tasten und Maus sendet, gegen mwt.info (unter headless sway, wie make test)
make pixel      # was mwt zeichnet, per Screenshot (unter headless sway, wie make test)
make mouse      # was mwt in jedem Mausmodus meldet (unter headless sway, wie make test)
make keyboard   # Dead Keys, Compose und Key-Repeat (unter headless sway, wie make test)
make test       # Black-Box-Tests unter headless sway (braucht sway, wtype, wl-clipboard, grim und vinput aus mwc)
make width      # wo mwt und bash, vim oder tmux Zeichenbreiten verschieden zählen (braucht zusätzlich vim und tmux)
make evil       # alle Tests plus Härtefälle in einem Podman-Container, auch mit Sanitizern
sudo make install   # installiert mwt nach /usr/local und die terminfo nach /usr/share/terminfo
sudo make uninstall # entfernt beides wieder
```

`make replay` holt die Dateien beim ersten Lauf aus dem Netz nach `tests/vt100`. Sie sind wegen ihrer unklaren Lizenz nicht im Repository. Der Container von `make evil` hat kein Netz und überspringt den Replay, wenn sie fehlen: `tests/replay.sh --fetch` einmal auf dem Host holt sie.

Ohne Installation genügt `tic -x mwt.info`. Das legt den Eintrag unter `~/.terminfo` ab. Fehlt der Eintrag, setzt mwt `TERM=xterm-256color` und gibt dazu eine Warnung aus.

## Aufruf

```
mwt [-f font] [-a app-id] [-T titel] [-g SPALTENxZEILEN] [-e] [befehl [args...]]
```

* `-f "JetBrains Mono:size=12"` übernimmt ein fontconfig-Pattern.
* `-a` setzt die app_id, Standard ist `mwt`. Darüber ordnet die mwmrc Farben und Position zu.
* `-g` nimmt 1x1 bis 4096x2048, solange der Fensterpuffer unter 2 GiB bleibt. Bei einer Schrift mit 9x18 Pixeln reicht das bis etwa 1800x1800, darüber lehnt mwt mit Code 2 ab. Größer als 4096x2048 wird das Raster auch im größten Fenster nicht.
* Ohne Befehl startet mwt `$SHELL`.

Alles Weitere, also Farben, Schrift, Padding und Scrollback, steht in `config.h` und wird beim Neubauen übernommen.

## Bedienung

| Eingabe | Wirkung |
|---|---|
| Linke Maustaste ziehen | Auswahl, wird beim Loslassen in die Primary Selection kopiert |
| Doppel-/Dreifachklick | Wort/Zeile auswählen |
| Rechtsklick | Menü: Kopieren, Einfügen, Alles markieren. Pfeiltasten und Return gehen auch, Escape schließt es |
| Shift+Linksklick | Auswahl erweitern |
| Mittelklick, Shift+Insert | Primary Selection einfügen |
| Ctrl+Shift+A | alles markieren: Scrollback und Bildschirm, im Alternate Screen nur den Bildschirm |
| Ctrl+Shift+C / V | Zwischenablage kopieren / einfügen |
| Shift+PgUp / PgDn | Scrollback (im Alternate Screen gehen sie ans Programm) |
| Mausrad | Scrollback |
| Shift+Mausrad | Scrollback seitenweise |
| Shift+Pos1 / Ende | an den Anfang / das Ende des Scrollbacks (nicht, wenn ein Programm Alternate Screen oder Maus nutzt) |
| Ctrl+Shift+Plus / Minus / 0 | Schrift größer / kleiner / zurücksetzen |
| Shift + Maus | Auswahl und Menü auch dann, wenn ein Programm die Maus übernommen hat |

## Aufbau

| Datei | Inhalt |
|---|---|
| `vt.c`, `vt.h` | Parser nach Paul Williams' DEC-Zustandsautomat, Grid, Scrollback. Kein Wayland-Code, daher headless testbar. |
| `font.c` | fontconfig/FreeType, vier Schnitte, Fallback-Fonts für fehlende Glyphen, Glyph-Cache |
| `mwt.c` | Wayland (xdg-shell, xdg-decoration, wl_shm, wl_data_device, primary selection), Tastatur über xkbcommon inklusive Compose/Dead Keys, Maus, pty, Event-Loop |
| `mwt.info` | terminfo: deklariert nur, was `vt.c` wirklich kann |
| `test_vt.c` | Tests für den VT-Kern |
| `tests/replay.c` | spielt aufgezeichnete Terminalausgabe ab: am Stück, Byte für Byte, in zufälligen Stücken und mit Größenänderungen. Die ersten drei müssen gleich enden, und nach jedem Schreiben muss das Raster stimmen. `make replay REPORT=1` listet pro Datei, was mwt davon nicht kann. |
| `tests/` | Black-Box-Tests und Härtefälle, aus dem mt-Projekt übernommen. `MWC_DIR` nennt den mwc-Quellbaum, Standard ist `../../mwc`. |

Gezeichnet werden nur geänderte Zeilen. Bei Doppelpufferung wird vorher der zuletzt gezeigte Puffer kopiert. Die Ausgabe ist an Frame-Callbacks gekoppelt.

## Unterstützt

* VT100/VT220-Kern: Cursorbewegung, ED/EL/ECH, ICH/DCH, IL/DL, SU/SD, Scroll-Regionen, Origin Mode, Tabs, REP, DEC-Sondergrafik (G0/G1)
* SGR: Bold, Dim, Italic, Underline, Reverse, Invisible, Strike; 16, 256 und Truecolor (`;` und `:`-Form)
* Alternate Screen (47/1047/1049), Bracketed Paste, Maus (1000/1002/1003 + SGR 1006), Focus Events, DECSCUSR (Cursorform)
* Antworten auf DA, DA2, DSR/CPR, DECRQM, XTVERSION, Fenstergröße (CSI 18 t)
* OSC 0/2 (Fenstertitel)
* Wide Characters (CJK, Emoji als Umriss, sofern ein Fallback-Font sie hat)
* HiDPI über `wl_surface.preferred_buffer_scale` (Ganzzahl-Skalierung)

## Bewusst weggelassen

Tektronix, VT52, Sixel/ReGIS, DCS-Abfragen, Reflow beim Resize, Ligaturen, bidi, kombinierende Zeichen (werden verworfen), farbige Emoji-Bitmaps, Subpixel-AA, fraktionale Skalierung, Drag and Drop, OSC 52.

## Hinweise zu mwc

* Rahmen kommen über xdg-decoration (server-side).
* Größenschritte: xdg-shell kennt keine Schrittweite wie `WM_NORMAL_HINTS`. Ein freies Fenster rastet mwt deshalb selbst auf ganze Zellen ein. Maximiert, im Vollbild oder gekachelt übernimmt mwt die vorgegebene Größe und füllt den Rest mit der Hintergrundfarbe. Der Resize-Umriss und die Größenanzeige von mwc zeigen deshalb Pixel statt Spalten und Zeilen.
* Bei ssh auf Hosts ohne mwt-terminfo: `TERM=xterm-256color ssh …` verwenden oder die terminfo dort mit `infocmp -x mwt | ssh host tic -x -` installieren.
