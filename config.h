/* config.h - compile-time configuration, edit and rebuild; the colors below,
 * the 16 of the palette too, and min_contrast can also be set in
 * ~/.config/mwt/mwtrc (rc.c) */

static const char *font_spec = "monospace:size=11";
static const char *default_app_id = "mwt";
static const char *default_title = "mwt";
static const char *term_name = "mwt";      /* falls back to xterm-256color
                                             * if no terminfo entry exists */
static const int default_cols = 80;
static const int default_rows = 24;
static const int padding = 2;               /* pixels, logical */
static const int histsize = 5000;           /* scrollback lines */
static const int wheel_lines = 3;
static const double touchpad_step = 10;     /* touchpad travel, in surface pixels, for
                                             * one step of wheel_lines */
static const int double_click_ms = 400;     /* a second click on the cell sooner is a
                                             * double click; a third, a triple */
static const double zoom_min_pt = 4, zoom_max_pt = 96; /* Ctrl+Shift+Plus and Minus */

/* DECRQCRA lets programs read the screen, as xterm's GetChecksum, so it is off.
 * esctest checks with it: make esctest builds mwt-esctest with it on. */
#ifdef MWT_ALLOW_CHECKSUM
static const bool allow_checksum = true;
#else
static const bool allow_checksum = false;
#endif

static const uint32_t color_fg = 0xdcdccc;
static const uint32_t color_bg = 0x1e1e1e;
static const uint32_t color_cursor = 0xdcdccc;
static const uint32_t color_sel_fg = 0x1e1e1e;
static const uint32_t color_sel_bg = 0xa0a0a0;

/* Text whose contrast to its background is below this (WCAG ratio, 1 to 21)
 * is drawn in color_fg or color_bg, whichever stands out more: black on a
 * dark green bar of a light theme, white on a pastel blue of a dark one.
 * 1 or less: off. */
static const float min_contrast = 3.0f;

static const uint32_t colors16[16] = {
	0x1e1e1e, 0xcc6666, 0x8fa86a, 0xd8b45e, 0x6c8fb8, 0xa87cb0, 0x6aa8a0, 0xc8c8c8,
	0x5a5a5a, 0xe08080, 0xa8c88a, 0xf0d080, 0x90b0d8, 0xc89ad0, 0x8ac8c0, 0xf4f4f4,
};
