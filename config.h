/* config.h - compile-time configuration, edit and rebuild */

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

static const uint32_t color_fg = 0xdcdccc;
static const uint32_t color_bg = 0x1e1e1e;
static const uint32_t color_cursor = 0xdcdccc;
static const uint32_t color_sel_fg = 0x1e1e1e;
static const uint32_t color_sel_bg = 0xa0a0a0;

static const uint32_t colors16[16] = {
	0x1e1e1e, 0xcc6666, 0x8fa86a, 0xd8b45e, 0x6c8fb8, 0xa87cb0, 0x6aa8a0, 0xc8c8c8,
	0x5a5a5a, 0xe08080, 0xa8c88a, 0xf0d080, 0x90b0d8, 0xc89ad0, 0x8ac8c0, 0xf4f4f4,
};
