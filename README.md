# mwt – minimal Wayland terminal

Ein kleiner Terminal-Emulator für Wayland, geschrieben in C. mwt ist ein eigenständiger xdg-shell-Client und braucht weder X11 noch Xwayland. Er läuft in mwc und in jedem anderen Compositor.

## Bauen

Abhängigkeiten: wayland-client, wayland-cursor, wayland-protocols, wayland-scanner, xkbcommon, fontconfig, freetype2.

```sh
make            # baut ./mwt
make check      # Tests für den VT-Kern (headless, mit ASan/UBSan)
sudo make install   # installiert nach /usr/local und kompiliert die terminfo
```

Ohne Installation genügt `tic -x mwt.info`. Das legt den Eintrag unter `~/.terminfo` ab. Fehlt der Eintrag, setzt mwt `TERM=xterm-256color` und gibt dazu eine Warnung aus.

## Aufruf

```
mwt [-f font] [-a app-id] [-T titel] [-g SPALTENxZEILEN] [-e] [befehl [args...]]
```

* `-f "JetBrains Mono:size=12"` übernimmt ein fontconfig-Pattern.
* `-a` setzt die app_id, Standard ist `mwt`. Darüber ordnet die mwmrc Farben und Position zu.
* Ohne Befehl startet mwt `$SHELL`.

Alles Weitere, also Farben, Schrift, Padding und Scrollback, steht in `config.h` und wird beim Neubauen übernommen.

## Bedienung

| Eingabe | Wirkung |
|---|---|
| Linke Maustaste ziehen | Auswahl, wird beim Loslassen in die Primary Selection kopiert |
| Doppel-/Dreifachklick | Wort/Zeile auswählen |
| Rechtsklick | Auswahl erweitern |
| Mittelklick, Shift+Insert | Primary Selection einfügen |
| Ctrl+Shift+C / V | Zwischenablage kopieren / einfügen |
| Shift+PgUp / PgDn, Mausrad | Scrollback |
| Ctrl+Shift+Plus / Minus / 0 | Schrift größer / kleiner / zurücksetzen |
| Shift + Maus | Auswahl auch dann, wenn ein Programm die Maus übernommen hat |

## Aufbau

| Datei | Inhalt |
|---|---|
| `vt.c`, `vt.h` | Parser nach Paul Williams' DEC-Zustandsautomat, Grid, Scrollback. Kein Wayland-Code, daher headless testbar. |
| `font.c` | fontconfig/FreeType, vier Schnitte, Fallback-Fonts für fehlende Glyphen, Glyph-Cache |
| `mwt.c` | Wayland (xdg-shell, xdg-decoration, wl_shm, wl_data_device, primary selection), Tastatur über xkbcommon inklusive Compose/Dead Keys, Maus, pty, Event-Loop |
| `mwt.info` | terminfo: deklariert nur, was `vt.c` wirklich kann |
| `test_vt.c` | Tests für den VT-Kern |

Gezeichnet werden nur geänderte Zeilen. Bei Doppelpufferung wird vorher der zuletzt gezeigte Puffer kopiert. Die Ausgabe ist an Frame-Callbacks gekoppelt.

## Unterstützt

* VT100/VT220-Kern: Cursorbewegung, ED/EL/ECH, ICH/DCH, IL/DL, SU/SD, Scroll-Regionen, Origin Mode, Tabs, REP, DEC-Sondergrafik (G0/G1)
* SGR: Bold, Dim, Italic, Underline, Reverse, Invisible, Strike; 16, 256 und Truecolor (`;` und `:`-Form)
* Alternate Screen (47/1047/1049), Bracketed Paste, Maus (1000/1002/1003 + SGR 1006), Focus Events, DECSCUSR (Cursorform)
* Antworten auf DA, DA2, DSR/CPR, XTVERSION, Fenstergröße (CSI 18 t)
* OSC 0/2 (Fenstertitel)
* Wide Characters (CJK, Emoji als Umriss, sofern ein Fallback-Font sie hat)
* HiDPI über `wl_surface.preferred_buffer_scale` (Ganzzahl-Skalierung)

## Bewusst weggelassen

Tektronix, VT52, Sixel/ReGIS, DCS-Abfragen, Reflow beim Resize, Ligaturen, bidi, kombinierende Zeichen (werden verworfen), farbige Emoji-Bitmaps, Subpixel-AA, fraktionale Skalierung, Drag and Drop, OSC 52.

## Hinweise zu mwc

* Rahmen kommen über xdg-decoration (server-side).
* Größenschritte: xdg-shell kennt keine Schrittweite wie `WM_NORMAL_HINTS`. Ein freies Fenster rastet mwt deshalb selbst auf ganze Zellen ein. Maximiert, im Vollbild oder gekachelt übernimmt mwt die vorgegebene Größe und füllt den Rest mit der Hintergrundfarbe. Der Resize-Umriss und die Größenanzeige von mwc zeigen deshalb Pixel statt Spalten und Zeilen.
* Bei ssh auf Hosts ohne mwt-terminfo: `TERM=xterm-256color ssh …` verwenden oder die terminfo dort mit `infocmp -x mwt | ssh host tic -x -` installieren.
