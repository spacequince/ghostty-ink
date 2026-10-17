// ghostty-ink: a terminal for the Kobo. libghostty-vt (VT state) + FBInk (e-ink drawing) + evdev keyboard + pty.
// Usage: ghostty-ink [-F font_index] [-M mult] [-K kbd_name_substr] [-C cols] [-R rows] [-X plugins_dir] [-q] -- cmd [args...]
// Environment: TERM=xterm-256color is set for the child.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <ghostty/vt.h>
#include "fbink.h"

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif
#define MAXDEV 8
#define BITS(x) (((x) + 7) / 8)

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static FILE *logf_;
#define LOG(...) do { if (logf_) { fprintf(logf_, "%.3f ", now()); fprintf(logf_, __VA_ARGS__); fputc('\n', logf_); fflush(logf_); fsync(fileno(logf_)); } } while (0)

// ---- keyboards ----------------------------------------------------------------------------
static int kfd[MAXDEV] = {-1, -1, -1, -1, -1, -1, -1, -1};   /* -1 = free slot */
static char kpath[MAXDEV][32];
static int nk;
static const char *kbd_match = NULL;
// Auto orientation: read the accelerometer's sysfs "xyz" (Kobo kx122: gravity along the panel axes, ~16000 = 1 g).
static char acc_dir[96];                   // /sys/class/input/eventN/device of the accelerometer, "" when there is none
static int rot_auto, rot_manual = -1, rot_auto_last = -1, rot_pending = -1;
static double rot_pend_t;
static int bfd = -1;                       // gpio-keys (page-turn buttons)
static int tfd = -1, pfd = -1;             // touchscreen, power key
static int t_single;                       // touch panel reports ABS_X/ABS_Y + BTN_TOUCH instead of multi-touch slots
static int tmaxx = 1, tmaxy = 1;           // touch axis ranges
static const char *inject_path;            // -I fifo: bytes written there are typed into the pty (debugging)
static int ifd = -1;
static int btn_exit = 193, btn_bottom = 194; // evdev codes; override with -T/-D (all button events are logged)

static FBInkState fst;
static int has(const unsigned char *b, int bit) { return b[bit / 8] & (1 << (bit % 8)); }

static void scan_keyboards(void) {
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	if (!d) return;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char path[64];
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int known = 0;
		for (int i = 0; i < nk; i++) if (kfd[i] >= 0 && !strcmp(kpath[i], path)) known = 1;
		if (known) continue;
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		unsigned char keys[BITS(KEY_MAX + 1)] = {0};
		char name[64] = "";
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
		ioctl(fd, EVIOCGNAME(sizeof name), name);
		if (strstr(name, "accel")) {
			char xyz[140];
			snprintf(xyz, sizeof xyz, "/sys/class/input/%s/device/xyz", e->d_name);
			if (!acc_dir[0] && access(xyz, R_OK) == 0) {
				snprintf(acc_dir, sizeof acc_dir, "/sys/class/input/%s/device", e->d_name);
				LOG("accelerometer %s (%s)", name, acc_dir);
			}
			close(fd);
			continue;
		}
		if (!strncmp(name, "gpio-keys", 9)) {
			// The page-turn buttons: top = exit, bottom = Morse keyboard (see handle_button).
			if (bfd < 0) { ioctl(fd, EVIOCGRAB, 1); bfd = fd; LOG("grabbed buttons %s (%s)", name, path); }
			else close(fd);
			continue;
		}
		unsigned char abs[BITS(ABS_MAX + 1)] = {0};
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
		int mt = has(abs, ABS_MT_POSITION_X) && has(abs, ABS_MT_POSITION_Y);
		int st = has(abs, ABS_X) && has(abs, ABS_Y) && has(keys, BTN_TOUCH);   // single-touch panel (some Kindles)
		if (strstr(name, "Touchscreen") || mt || st) {
			if (tfd < 0) {
				struct input_absinfo ai;
				int ax = mt ? ABS_MT_POSITION_X : ABS_X, ay = mt ? ABS_MT_POSITION_Y : ABS_Y;
				if (ioctl(fd, EVIOCGABS(ax), &ai) == 0) tmaxx = ai.maximum > 0 ? ai.maximum : 1;
				if (ioctl(fd, EVIOCGABS(ay), &ai) == 0) tmaxy = ai.maximum > 0 ? ai.maximum : 1;
				t_single = !mt;
				ioctl(fd, EVIOCGRAB, 1); tfd = fd;
				LOG("grabbed touchscreen %s (%s) max %dx%d%s", name, path, tmaxx, tmaxy, t_single ? " (single-touch)" : "");
			} else close(fd);
			continue;
		}
		if (strstr(name, "pwrkey") || (has(keys, KEY_POWER) && !has(keys, KEY_A))) {
			if (pfd < 0) { ioctl(fd, EVIOCGRAB, 1); pfd = fd; LOG("grabbed power key %s (%s)", name, path); }
			else close(fd);
			continue;
		}
		int ok = has(keys, KEY_A) && has(keys, KEY_SPACE) && has(keys, KEY_ENTER) && strncmp(name, "gpio", 4);
		if (ok && kbd_match && !strstr(name, kbd_match)) ok = 0;
		if (!ok) { close(fd); continue; }
		ioctl(fd, EVIOCGRAB, 1);
		int slot = -1;
		for (int i = 0; i < nk; i++) if (kfd[i] < 0) { slot = i; break; }
		if (slot < 0) {
			if (nk >= MAXDEV) { close(fd); continue; }
			slot = nk++;
		}
		kfd[slot] = fd;
		snprintf(kpath[slot], sizeof kpath[slot], "%s", path);
		LOG("grabbed keyboard %s (%s)", name, path);
	}
	closedir(d);
}

// ---- globals -----------------------------------------------------------------------------
static int ptyfd = -1;
static pid_t child = -1;
static GhosttyTerminal term;
static GhosttyRenderState rs;
static GhosttyRenderStateRowIterator rowit;
static GhosttyRenderStateRowCells rowcells;
static int fb = -1;
static FBInkConfig cfg;
static FBInkState fst;
static int cols, rows;

static void pty_write(const void *p, size_t n) {
	const char *c = p;
	while (n) {
		ssize_t w = write(ptyfd, c, n);
		if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; }
		c += w; n -= w;
	}
}

static void on_write_pty(GhosttyTerminal t, void *ud, const uint8_t *data, size_t len) {
	(void)t; (void)ud;
	pty_write(data, len);
}

// ---- keymap ------------------------------------------------------------------------------
static const char lo[] = "##1234567890-=##qwertyuiop[]##asdfghjkl;'`#\\zxcvbnm,./";
static const char up[] = "##!@#$%^&*()_+##QWERTYUIOP{}##ASDFGHJKL:\"~#|ZXCVBNM<>?";
static int mod_shift, mod_ctrl, mod_alt;

static int app_cursor(void) {
	GhosttyTerminalModeConfig m = {0};
	m.mode = GHOSTTY_MODE_DECCKM;
	if (ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_MODE, &m) != GHOSTTY_SUCCESS) return 0;
	return m.value;
}

// Returns bytes written to out (0 = nothing / handled locally).
static int encode_key(int code, char *out) {
	int n = 0;
	int alt = mod_alt;
	const char *seq = NULL;
	char tmp[16];
	int mod = 1 + (mod_shift ? 1 : 0) + (mod_alt ? 2 : 0) + (mod_ctrl ? 4 : 0);
	int cur = app_cursor();
	int fnum = 0;
	char fin = 0;
	switch (code) {
	case KEY_UP: fin = 'A'; break;
	case KEY_DOWN: fin = 'B'; break;
	case KEY_RIGHT: fin = 'C'; break;
	case KEY_LEFT: fin = 'D'; break;
	case KEY_HOME: fin = 'H'; break;
	case KEY_END: fin = 'F'; break;
	case KEY_INSERT: fnum = 2; break;
	case KEY_DELETE: fnum = 3; break;
	case KEY_PAGEUP: fnum = 5; break;
	case KEY_PAGEDOWN: fnum = 6; break;
	case KEY_F1: fin = 'P'; break;
	case KEY_F2: fin = 'Q'; break;
	case KEY_F3: fin = 'R'; break;
	case KEY_F4: fin = 'S'; break;
	case KEY_F5: fnum = 15; break;
	case KEY_F6: fnum = 17; break;
	case KEY_F7: fnum = 18; break;
	case KEY_F8: fnum = 19; break;
	case KEY_F9: fnum = 20; break;
	case KEY_F10: fnum = 21; break;
	case KEY_F11: fnum = 23; break;
	case KEY_F12: fnum = 24; break;
	}
	if (fin) {
		int ss3 = (code >= KEY_F1 && code <= KEY_F4) || cur;
		if (mod > 1) n = snprintf(out, 16, "\033[1;%d%c", mod, fin);
		else if (ss3) n = snprintf(out, 16, "\033O%c", fin);
		else n = snprintf(out, 16, "\033[%c", fin);
		return n;
	}
	if (fnum) {
		if (mod > 1) n = snprintf(out, 16, "\033[%d;%d~", fnum, mod);
		else n = snprintf(out, 16, "\033[%d~", fnum);
		return n;
	}
	switch (code) {
	case KEY_ENTER: case KEY_KPENTER: seq = "\r"; break;
	case KEY_BACKSPACE: seq = "\x7f"; break;
	case KEY_TAB: seq = mod_shift ? "\033[Z" : "\t"; break;
	case KEY_ESC: seq = "\033"; break;
	case KEY_SPACE: seq = mod_ctrl ? "\0" : " "; break;
	}
	if (seq) {
		if (code == KEY_SPACE && mod_ctrl) { tmp[0] = 0; n = 1; }
		else n = snprintf(tmp, sizeof tmp, "%s", seq), n = n;
		if (alt) { out[0] = 033; memcpy(out + 1, tmp, n ? n : 1); return n + 1; }
		memcpy(out, tmp, n ? n : 1);
		return n;
	}
	if (code < (int)sizeof lo - 1 && !(code < 2 || code == 14 || code == 15 || code == 28 || code == 29 || code == 42)) {
		char c = mod_shift ? up[code] : lo[code];
		if (mod_ctrl) {
			char b = lo[code];
			if (b >= 'a' && b <= 'z') c = b - 'a' + 1;
			else if (b == '[') c = 27;
			else if (b == '\\') c = 28;
			else if (b == ']') c = 29;
			else if (b == '6' || b == '`') c = (b == '6') ? 30 : 0;
			else if (b == '-' || b == '/') c = (b == '-') ? 31 : 31;
			else if (b == '2') c = 0;
		}
		if (alt) { out[0] = 033; out[1] = c; return 2; }
		out[0] = c;
		return 1;
	}
	return 0;
}

// ---- rendering ---------------------------------------------------------------------------
static GhosttyColorRgb defbg = {0xff, 0xff, 0xff}, deffg = {0, 0, 0};
static uint8_t last_fg[3] = {1, 1, 1}, last_bg[3] = {1, 1, 1};

// ---- colour treatment: 0 as sent, 1 readable (contrast fix), 2 grayscale, 3 black & white ----
static int color_mode = 1;
static float lin_c(uint8_t c) { float x = c / 255.0f; return x <= 0.04045f ? x / 12.92f : __builtin_powf((x + 0.055f) / 1.055f, 2.4f); }
static float lum_of(GhosttyColorRgb c) { return 0.2126f * lin_c(c.r) + 0.7152f * lin_c(c.g) + 0.0722f * lin_c(c.b); }
static float contrast_of(float a, float b) { if (a < b) { float t = a; a = b; b = t; } return (a + 0.05f) / (b + 0.05f); }
static GhosttyColorRgb mix_c(GhosttyColorRgb a, GhosttyColorRgb b, float t) {
	return (GhosttyColorRgb){(uint8_t)(a.r + (b.r - a.r) * t + 0.5f), (uint8_t)(a.g + (b.g - a.g) * t + 0.5f), (uint8_t)(a.b + (b.b - a.b) * t + 0.5f)};
}
// Move fg toward black or white (whichever has more room against bg) until the contrast ratio is at least `want`.
static void ensure_contrast(GhosttyColorRgb *fg, GhosttyColorRgb bg, float want) {
	float lb = lum_of(bg);
	GhosttyColorRgb target = lb > 0.179f ? (GhosttyColorRgb){0, 0, 0} : (GhosttyColorRgb){255, 255, 255};
	if (contrast_of(lum_of(*fg), lb) >= want) return;
	for (int i = 1; i <= 10; i++) {
		GhosttyColorRgb c = mix_c(*fg, target, i / 10.0f);
		if (contrast_of(lum_of(c), lb) >= want) { *fg = c; return; }
	}
	*fg = target;
}
static GhosttyColorRgb gray_of(GhosttyColorRgb c) { uint8_t y = (uint8_t)(0.299f * c.r + 0.587f * c.g + 0.114f * c.b + 0.5f); return (GhosttyColorRgb){y, y, y}; }
static void convert_colors(GhosttyColorRgb *fg, GhosttyColorRgb *bg) {
	if (color_mode == 0) return;
	static GhosttyColorRgb mf, mb, rf, rb;
	static int mm = -1;
	if (mm == color_mode && !memcmp(&mf, fg, 3) && !memcmp(&mb, bg, 3)) { *fg = rf; *bg = rb; return; }
	GhosttyColorRgb f = *fg, b = *bg;
	switch (color_mode) {
	case 1: ensure_contrast(&f, b, 4.5f); break;
	case 2: f = gray_of(f); b = gray_of(b); ensure_contrast(&f, b, 4.5f); break;
	case 3: {
		bool light = gray_of(b).r > 127;
		b = light ? (GhosttyColorRgb){255, 255, 255} : (GhosttyColorRgb){0, 0, 0};
		f = light ? (GhosttyColorRgb){0, 0, 0} : (GhosttyColorRgb){255, 255, 255};
		break;
	}
	}
	mf = *fg; mb = *bg; mm = color_mode; rf = f; rb = b;
	*fg = f; *bg = b;
}

static void set_pen(GhosttyColorRgb fg, GhosttyColorRgb bg) {
	if (memcmp(last_fg, &fg, 3)) { fbink_set_fg_pen_rgba(fg.r, fg.g, fg.b, 0xff, false, false); memcpy(last_fg, &fg, 3); }
	if (memcmp(last_bg, &bg, 3)) { fbink_set_bg_pen_rgba(bg.r, bg.g, bg.b, 0xff, false, false); memcpy(last_bg, &bg, 3); }
}

static int put_utf8(char *o, uint32_t cp) {
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xc0 | (cp >> 6); o[1] = 0x80 | (cp & 63); return 2; }
	if (cp < 0x10000) { o[0] = 0xe0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 63); o[2] = 0x80 | (cp & 63); return 3; }
	o[0] = 0xf0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 63); o[2] = 0x80 | ((cp >> 6) & 63); o[3] = 0x80 | (cp & 63);
	return 4;
}

// ---- TrueType cell renderer (anti-aliased; used when -f font.ttf is given) ----------------------
// Glyphs are rasterised with stb_truetype, cached by (codepoint, bold), and composited straight into the
// framebuffer; refreshes then go through fbink_refresh. Without -f we fall back to FBInk's bitmap fonts.
// An optional symbols font (-s, e.g. Symbols Nerd Font Mono) supplies glyphs the main font lacks, scaled to
// fit one cell. The screen can be rotated in 90 degree steps (rotation = clockwise degrees).
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static bool use_ttf;
static bool glyph_binary;   // draw 1-bit glyphs (fast DU refreshes); the quality pass redraws anti-aliased
static unsigned char *ttf_data[4];     // regular, bold, symbols, emoji (monochrome outlines, e.g. Noto Emoji)
static stbtt_fontinfo ttf_fi[4];
static bool have_bold_font, have_sym_font, have_emoji_font;
static float ttf_scale[2], sym_scale;
static int sym_yadj;
static int cell_w, cell_h, ascent_px, ttf_px = 30;
static const char *font_reg_path, *font_bold_path, *font_sym_path, *font_emoji_path;
static unsigned char *fbmem;
static size_t fbmem_size;
static int fb_stride, scr_w, scr_h, lw, lh, grid_ox, grid_oy, rotation;
static int forced_cols, forced_rows;
static int sh_r, sh_g, sh_b, sh_a;
static uint8_t gamma_lut[256];

typedef struct { uint32_t key; int w, h, xoff, yoff; uint8_t *cov; } Glyph;
#define GCACHE 4096
static Glyph gcache[GCACHE];   // key = (cp << 1 | bold) + 1; 0 = empty

static unsigned char *slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc(n);
	if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
	fclose(f);
	return b;
}

// (Re)compute scales and cell metrics for the current ttf_px; drops cached glyphs.
static bool ttf_metrics(void) {
	for (int i = 0; i < GCACHE; i++) { free(gcache[i].cov); gcache[i] = (Glyph){0}; }
	ttf_scale[0] = stbtt_ScaleForPixelHeight(&ttf_fi[0], ttf_px);
	if (have_bold_font) ttf_scale[1] = stbtt_ScaleForPixelHeight(&ttf_fi[1], ttf_px);
	int asc, desc, gap, adv, lsb;
	stbtt_GetFontVMetrics(&ttf_fi[0], &asc, &desc, &gap);
	stbtt_GetCodepointHMetrics(&ttf_fi[0], '0', &adv, &lsb);
	ascent_px = (int)(asc * ttf_scale[0] + 0.5f);
	cell_h = (int)((asc - desc) * ttf_scale[0] + 0.999f) + 2;
	cell_w = (int)(adv * ttf_scale[0] + 0.5f);
	if (have_sym_font) {
		int a2, d2, g2, adv2, lsb2;
		stbtt_GetFontVMetrics(&ttf_fi[2], &a2, &d2, &g2);
		stbtt_GetCodepointHMetrics(&ttf_fi[2], 0xe0b0, &adv2, &lsb2);
		if (adv2 <= 0) adv2 = a2 - d2;
		sym_scale = (float)cell_w / adv2;              // symbol advance == one cell
		float base2 = (cell_h - (a2 - d2) * sym_scale) / 2 + a2 * sym_scale;   // centre the symbol box in the cell
		sym_yadj = (int)(base2 + 0.5f) - ascent_px;
	}
	return cell_w > 0 && cell_h > 0;
}

static bool ttf_init(void) {
	ttf_data[0] = slurp(font_reg_path);
	if (!ttf_data[0] || !stbtt_InitFont(&ttf_fi[0], ttf_data[0], stbtt_GetFontOffsetForIndex(ttf_data[0], 0))) return false;
	if (font_bold_path && (ttf_data[1] = slurp(font_bold_path)) &&
	    stbtt_InitFont(&ttf_fi[1], ttf_data[1], stbtt_GetFontOffsetForIndex(ttf_data[1], 0)))
		have_bold_font = true;
	if (font_sym_path && (ttf_data[2] = slurp(font_sym_path)) &&
	    stbtt_InitFont(&ttf_fi[2], ttf_data[2], stbtt_GetFontOffsetForIndex(ttf_data[2], 0)))
		have_sym_font = true;
	if (font_emoji_path && (ttf_data[3] = slurp(font_emoji_path)) &&
	    stbtt_InitFont(&ttf_fi[3], ttf_data[3], stbtt_GetFontOffsetForIndex(ttf_data[3], 0)))
		have_emoji_font = true;
	for (int i = 0; i < 256; i++) gamma_lut[i] = (uint8_t)(255.0 * __builtin_pow(i / 255.0, 0.75) + 0.5);
	return ttf_metrics();
}

static Glyph *get_glyph(uint32_t cp, bool bold, int ncells) {
	uint32_t key = (((cp | (ncells == 2 ? 1u << 24 : 0)) << 1) | bold) + 1;
	uint32_t h = (key * 2654435761u) % GCACHE;
	for (int probe = 0; probe < GCACHE; probe++, h = (h + 1) % GCACHE) {
		Glyph *g = &gcache[h];
		if (g->key == key) return g;
		if (g->key) continue;
		stbtt_fontinfo *fi = NULL;
		float sc = 0;
		int yadj = 0, gi = 0;
		bool synth = false;
		int xadj_emoji = 0;
		if (bold && have_bold_font && (gi = stbtt_FindGlyphIndex(&ttf_fi[1], cp))) { fi = &ttf_fi[1]; sc = ttf_scale[1]; }
		else if ((gi = stbtt_FindGlyphIndex(&ttf_fi[0], cp))) { fi = &ttf_fi[0]; sc = ttf_scale[0]; synth = bold; }
		else if (have_sym_font && (gi = stbtt_FindGlyphIndex(&ttf_fi[2], cp))) { fi = &ttf_fi[2]; sc = sym_scale; yadj = sym_yadj; }
		else if (have_emoji_font && (gi = stbtt_FindGlyphIndex(&ttf_fi[3], cp))) {
			// Emoji: fit the glyph's box into 1 or 2 cells (square), centred, whatever the font's own proportions
			fi = &ttf_fi[3];
			int ex0, ey0, ex1, ey1;
			stbtt_GetGlyphBitmapBox(fi, gi, 1.0f, 1.0f, &ex0, &ey0, &ex1, &ey1);
			float bw = ex1 - ex0 > 0 ? ex1 - ex0 : 1, bh = ey1 - ey0 > 0 ? ey1 - ey0 : 1;
			float box = (float)(ncells * cell_w < cell_h ? ncells * cell_w : cell_h) * 0.92f;
			sc = box / (bw > bh ? bw : bh);
			stbtt_GetGlyphBitmapBox(fi, gi, sc, sc, &ex0, &ey0, &ex1, &ey1);
			yadj = (cell_h - (ey1 - ey0)) / 2 - (ascent_px + ey0);                 // vertical centre in the cell
			xadj_emoji = (ncells * cell_w - (ex1 - ex0)) / 2 - ex0;                 // horizontal centre in the cell(s)
		}
		else { fi = &ttf_fi[0]; sc = ttf_scale[0]; gi = 0; }   // missing everywhere: regular .notdef
		int x0, y0, x1, y1;
		stbtt_GetGlyphBitmapBox(fi, gi, sc, sc, &x0, &y0, &x1, &y1);
		int w = x1 - x0, hh = y1 - y0;
		{ static int nlog; if (cp > 0x7f && nlog < 80) { nlog++; LOG("glyph U+%04X bold=%d font=%d gi=%d box %dx%d off %d,%d scale %.3f", cp, bold, fi == &ttf_fi[2] ? 2 : fi == &ttf_fi[1] ? 1 : 0, gi, w, hh, x0, y0, sc); } }
		g->key = key; g->xoff = x0 + xadj_emoji; g->yoff = y0 + yadj;
		if (w <= 0 || hh <= 0) { g->w = g->h = 0; g->cov = NULL; return g; }
		int ww = w + (synth ? 1 : 0);
		uint8_t *buf = calloc(ww, hh);
		uint8_t *tmp = calloc(w, hh);
		stbtt_MakeGlyphBitmap(fi, tmp, w, hh, w, sc, sc, gi);
		for (int yy = 0; yy < hh; yy++)
			for (int xx = 0; xx < ww; xx++) {
				int a = xx < w ? tmp[yy * w + xx] : 0;
				if (synth && xx > 0) { int b = tmp[yy * w + xx - 1]; if (b > a) a = b; }
				buf[yy * ww + xx] = gamma_lut[a];
			}
		free(tmp);
		g->w = ww; g->h = hh; g->cov = buf;
		return g;
	}
	return &gcache[0];
}

static inline uint32_t pack_rgb(int r, int g, int b) {
	return ((uint32_t)r << sh_r) | ((uint32_t)g << sh_g) | ((uint32_t)b << sh_b) | (0xffu << sh_a);
}

// Logical (rotated) pixel -> framebuffer pixel.
static inline uint32_t *pix(int lx, int ly) {
	int x, y;
	switch (rotation) {
	case 90: x = scr_w - 1 - ly; y = lx; break;
	case 180: x = scr_w - 1 - lx; y = scr_h - 1 - ly; break;
	case 270: x = ly; y = scr_h - 1 - lx; break;
	default: x = lx; y = ly;
	}
	return (uint32_t *)(fbmem + (size_t)y * fb_stride) + x;
}

// Logical rectangle -> framebuffer rectangle.
static void phys_rect(int lx, int ly, int w, int h, int *px, int *py, int *pw, int *ph) {
	switch (rotation) {
	case 90: *px = scr_w - (ly + h); *py = lx; *pw = h; *ph = w; break;
	case 180: *px = scr_w - (lx + w); *py = scr_h - (ly + h); *pw = w; *ph = h; break;
	case 270: *px = ly; *py = scr_h - (lx + w); *pw = h; *ph = w; break;
	default: *px = lx; *py = ly; *pw = w; *ph = h;
	}
	if (*px < 0) { *pw += *px; *px = 0; }
	if (*py < 0) { *ph += *py; *py = 0; }
	if (*px + *pw > scr_w) *pw = scr_w - *px;
	if (*py + *ph > scr_h) *ph = scr_h - *py;
}

static void fill_fb(GhosttyColorRgb c) {
	uint32_t p = pack_rgb(c.r, c.g, c.b);
	for (int y = 0; y < scr_h; y++) {
		uint32_t *d = (uint32_t *)(fbmem + (size_t)y * fb_stride);
		for (int x = 0; x < scr_w; x++) d[x] = p;
	}
}

static void draw_cell_n(int col, int row, uint32_t cp, GhosttyColorRgb fg, GhosttyColorRgb bg, bool bold, int underline, bool strike, int ncells) {
	int x0 = grid_ox + col * cell_w, y0 = grid_oy + row * cell_h;
	int cw = cell_w * ncells;
	if (x0 < 0 || y0 < 0 || x0 + cw > lw || y0 + cell_h > lh) return;
	uint32_t bgp = pack_rgb(bg.r, bg.g, bg.b), fgp = pack_rgb(fg.r, fg.g, fg.b);
	for (int yy = 0; yy < cell_h; yy++)
		for (int xx = 0; xx < cw; xx++) *pix(x0 + xx, y0 + yy) = bgp;
	if (cp > 0x20) {
		Glyph *g = get_glyph(cp, bold, ncells);
		for (int yy = 0; yy < g->h; yy++) {
			int py = ascent_px + g->yoff + yy;
			if (py < 0 || py >= cell_h) continue;
			for (int xx = 0; xx < g->w; xx++) {
				int px = g->xoff + xx;
				if (px < 0 || px >= cw) continue;
				int a = g->cov[yy * g->w + xx];
				if (glyph_binary) a = a >= 110 ? 255 : 0;
				if (!a) continue;
				if (a >= 250) *pix(x0 + px, y0 + py) = fgp;
				else *pix(x0 + px, y0 + py) = pack_rgb(bg.r + (fg.r - bg.r) * a / 255, bg.g + (fg.g - bg.g) * a / 255, bg.b + (fg.b - bg.b) * a / 255);
			}
		}
	}
	if (underline) for (int xx = 0; xx < cw; xx++) {
		int py = ascent_px + 3;
		if (py < cell_h) *pix(x0 + xx, y0 + py) = fgp;
	}
	if (strike) for (int xx = 0; xx < cw; xx++)
		*pix(x0 + xx, y0 + ascent_px * 55 / 100) = fgp;
}

static void draw_cell(int col, int row, uint32_t cp, GhosttyColorRgb fg, GhosttyColorRgb bg, bool bold, int underline, bool strike) {
	draw_cell_n(col, row, cp, fg, bg, bold, underline, strike, 1);
}

// ---- refresh settings (menu: Refresh), persisted in settings.conf ----
static int rf_wfm = 0;              // typing waveform: 0 DU (fast, 1-bit glyphs), 1 A2 (fastest), 2 GL16 (smooth, anti-aliased at once)
static int rf_batch_ms = 60;        // changes are collected for this long before one refresh
static int rf_quality_ms = 600;     // idle time before the anti-aliased clean-up pass (0 = never)
static int rf_flash_rows = 160;     // rows redrawn before a long idle ends in a flashing clean-up (0 = never)
static const int rf_batch_opts[] = {30, 60, 120, 250};
static const int rf_quality_opts[] = {0, 300, 600, 1500, 3000};
static const int rf_flash_opts[] = {0, 80, 160, 400};
#define NOPTS(a) ((int)(sizeof(a) / sizeof *(a)))
static WFM_MODE_INDEX_T typing_wfm(void) { return rf_wfm == 1 ? WFM_A2 : rf_wfm == 2 ? WFM_GL16 : WFM_DU; }
#define MAXROWS 512
static bool rr[MAXROWS];            // rows drawn since the last refresh (refreshed as separate runs)
static bool rr_any;
static bool q_rows[MAXROWS];        // rows drawn in fast binary mode that still need an anti-aliased quality pass
static bool q_any, aa_pass, q_force, glyph_binary;
static bool slow_row[MAXROWS];      // row has coloured cell backgrounds: needs a real grayscale/colour waveform, not DU
static int burst_slow;              // slow rows drawn since the last quality pass
static inline void mark_rows(int y) { if (y >= 0 && y < rows && y < MAXROWS) { rr[y] = true; rr_any = true; } }
static int cur_x = -1, cur_y = -1;
static int extra_dirty_row = -1;    // row to repaint on the next draw() (e.g. after the Morse indicator overlay)
static int bold_dark_fix = 1;

// Draw all dirty rows (or all rows if full); returns number of rows drawn.
static int draw(bool force_all) {
	if (ghostty_render_state_update(rs, term) != GHOSTTY_SUCCESS) return 0;
	GhosttyRenderStateDirty dirty;
	ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
	GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
	ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur);
	int cx = -1, cy = -1;
	if (cur.visible && cur.viewport_has_value) { cx = cur.viewport_x; cy = cur.viewport_y; }
	// a cursor-only move (arrow keys) does not mark any row dirty, but the cursor still has to be redrawn
	if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE && !force_all && extra_dirty_row < 0 && !q_force && cx == cur_x && cy == cur_y) return 0;
	glyph_binary = use_ttf && !aa_pass && rf_wfm != 2;

	GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
	ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
	defbg = colors.background; deffg = colors.foreground;

	bool cursor_moved = cx != cur_x || cy != cur_y;

	ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rowit);
	FBInkConfig c = cfg;
	c.no_refresh = true;
	int drawn = 0;
	int y = -1;
	while (ghostty_render_state_row_iterator_next(rowit)) {
		y++;
		bool rowdirty = force_all;
		if (!rowdirty) ghostty_render_state_row_get(rowit, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &rowdirty);
		if (!rowdirty && cursor_moved && (y == cy || y == cur_y)) rowdirty = true;
		if (y == extra_dirty_row) rowdirty = true;
		if (q_force) rowdirty = y < MAXROWS && q_rows[y];
		if (!rowdirty || y >= rows) continue;
		bool rowslow = false, rowbin = false;
		ghostty_render_state_row_get(rowit, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &rowcells);

		// Build runs of identical style.
		char buf[1024];
		int blen = 0, runstart = 0, x = 0;
		GhosttyColorRgb rfg = deffg, rbg = defbg;
		bool have = false;
		struct { int col; char s[1024]; GhosttyColorRgb fg, bg; } run;
		(void)run;
		while (ghostty_render_state_row_cells_next(rowcells)) {
			GhosttyStyle st = GHOSTTY_INIT_SIZED(GhosttyStyle);
			ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &st);
			GhosttyColorRgb fg = deffg, bg = defbg;
			GhosttyColorRgb t;
			if (ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &t) == GHOSTTY_SUCCESS) fg = t;
			if (ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &t) == GHOSTTY_SUCCESS) bg = t;
			convert_colors(&fg, &bg);
			GhosttyColorRgb bg_orig = bg;
			bool sel = false;
			ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_SELECTED, &sel);
			if (st.inverse != (sel || (x == cx && y == cy))) { t = fg; fg = bg; bg = t; }
			if (st.faint) { fg.r = (fg.r + bg.r) / 2; fg.g = (fg.g + bg.g) / 2; fg.b = (fg.b + bg.b) / 2; }
			if (st.invisible) fg = bg;

			uint32_t glen = 0;
			ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
			char glyph[32];
			int gl = 0;
			uint32_t cp0 = ' ';
			if (glen == 0) glyph[gl++] = ' ';
			else {
				uint32_t cps[8];
				uint32_t n = glen < 8 ? glen : 8;
				ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
				// One glyph per cell: only the base codepoint.
				cp0 = cps[0] ? cps[0] : ' ';
				gl = put_utf8(glyph, cp0);
				(void)n;
			}
			if (use_ttf) {
				bool colored = memcmp(&bg_orig, &defbg, 3) != 0 || st.inverse;
				glyph_binary = !aa_pass && !colored && rf_wfm != 2;
				rowslow |= colored; rowbin |= glyph_binary;
				GhosttyCell raw = 0;
				int wide = GHOSTTY_CELL_WIDE_NARROW;
				if (ghostty_render_state_row_cells_get(rowcells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw) == GHOSTTY_SUCCESS)
					ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
				if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) { x++; if (x >= cols) break; continue; }   // the wide cell before it drew both halves
				if (wide == GHOSTTY_CELL_WIDE_WIDE && x + 1 < cols) draw_cell_n(x, y, cp0, fg, bg, st.bold, st.underline, st.strikethrough, 2);
				else draw_cell(x, y, cp0, fg, bg, st.bold, st.underline, st.strikethrough);
				x++;
				if (x >= cols) break;
				continue;
			}
			bool same = have && !memcmp(&fg, &rfg, 3) && !memcmp(&bg, &rbg, 3);
			if (!same || blen + gl >= (int)sizeof buf - 1) {
				if (have && blen) {
					buf[blen] = 0;
					set_pen(rfg, rbg);
					c.col = runstart; c.row = y;
					fbink_print(fb, buf, &c);
				}
				blen = 0; runstart = x; rfg = fg; rbg = bg; have = true;
			}
			memcpy(buf + blen, glyph, gl); blen += gl;
			x++;
			if (x >= cols) break;
		}
		if (have && blen) {
			buf[blen] = 0;
			set_pen(rfg, rbg);
			c.col = runstart; c.row = y;
			fbink_print(fb, buf, &c);
		}
		mark_rows(y);
		if (use_ttf && y < MAXROWS) {
			slow_row[y] = rowslow && !aa_pass;
			if (rowslow && !aa_pass) burst_slow++;
			q_rows[y] = rowbin;
			if (rowbin) q_any = true;
		}
		drawn++;
		bool f = false;
		ghostty_render_state_row_set(rowit, GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &f);
	}
	// Previous cursor row may have been repainted above; remember the new position.
	if (cursor_moved) {
		if (cur_y >= 0 && cur_y < rows && cur_y != cy) { mark_rows(cur_y); if (use_ttf && cur_y < MAXROWS && glyph_binary) { q_rows[cur_y] = true; q_any = true; } }
		cur_x = cx; cur_y = cy;
	}
	extra_dirty_row = -1;
	ghostty_render_state_clean(rs);
	(void)bold_dark_fix;
	return drawn;
}

static bool refresh_flash;     // next refresh flashes (clears ghosting completely)
static void refresh_rows(WFM_MODE_INDEX_T wfm, int y0, int y1) {
	FBInkConfig c = cfg;
	c.wfm_mode = wfm;
	c.is_flashing = refresh_flash;
	int r;
	if (use_ttf) {
		int px, py, pw, ph;
		phys_rect(0, grid_oy + y0 * cell_h, lw, (y1 - y0 + 1) * cell_h, &px, &py, &pw, &ph);
		r = fbink_refresh(fb, py, px, pw, ph, &c);
	}
	else { c.row = y0; c.col = 0; r = fbink_grid_refresh(fb, cols, y1 - y0 + 1, &c); }
	if (r < 0) LOG("refresh failed rc=%d wfm=%d rows %d..%d", r, wfm, y0, y1);
}

// Refresh the rows drawn since the last refresh, each contiguous run separately (a typed character plus a
// distant status line must not turn into one screen-sized update), or the whole screen.
static void refresh_dirty(WFM_MODE_INDEX_T wfm, bool full_screen) {
	if (!rr_any && !full_screen) return;
	if (full_screen) refresh_rows(wfm, 0, rows - 1);
	else
		for (int y = 0; y < rows && y < MAXROWS;) {
			if (!rr[y]) { y++; continue; }
			int y1 = y;
			while (y1 + 1 < rows && y1 + 1 < MAXROWS && rr[y1 + 1] && slow_row[y1 + 1] == slow_row[y]) y1++;
			refresh_rows(slow_row[y] ? WFM_GC16 : wfm, y, y1);   // coloured rows can't use the 1-bit fast waveform
			y = y1 + 1;
		}
	memset(rr, 0, sizeof rr);
	rr_any = false;
}

// After typing settles: redraw the rows we drew as fast binary glyphs with anti-aliasing and a quality waveform.
static void quality_pass(void) {
	if (burst_slow * 10 >= rows * 3) {
		// Screen-wide coloured change (alt-screen app with a background colour, say): redraw everything
		// anti-aliased and flash-clear, otherwise the previous content shows through.
		aa_pass = true;
		draw(true);
		aa_pass = false;
		memset(q_rows, 0, sizeof q_rows);
		memset(slow_row, 0, sizeof slow_row);
		q_any = false; burst_slow = 0;
		refresh_flash = true;
		refresh_dirty(WFM_GC16, true);
		refresh_flash = false;
		return;
	}
	burst_slow = 0;
	if (!q_any) return;
	aa_pass = true; q_force = true;
	draw(false);
	q_force = false; aa_pass = false; q_any = false;
	refresh_dirty(WFM_GC16, false);
}

// ---- geometry, settings, frontlight, menu, touch, lock --------------------------------------
#include <ifaddrs.h>
#include <arpa/inet.h>
static volatile sig_atomic_t quit;    // (tentative definition; the signal handlers below set it)
static const char *settings_path;
static int exit_code;                 // 0 = reboot into Nickel, 42 = power off (see ghostty.sh)
static int set_bright = -1, set_warm = -1;   // saved frontlight levels; -1 = leave the frontlight alone
static int touch_mode = 2;            // 0 off, 1 scroll only, 2 scroll + mouse clicks
static int scroll_natural = 1;        // finger drags content (down = look at older output)
static int theme_idx;                 // 0 light, 1 dark
static bool locked, menu_open;
static int osk_mode;                  // on-screen keyboard: 0 auto (shown while no Bluetooth keyboard is connected), 1 always, 2 never
static bool osk_visible;
static bool plug_dirty;               // the plugin set changed (keyboard keys added/removed) while the menu was open
// two-finger swipe gestures (Menu > Touch): what each does
enum { GA_NONE, GA_SHOW_KB, GA_HIDE_KB, GA_CLEAR, GA_MENU, GA_COUNT };
static const char *gesture_names[GA_COUNT] = {"Nothing", "Show keyboard", "Hide keyboard", "Clear ghosting", "Open menu"};
static int gest_up = GA_SHOW_KB, gest_down = GA_HIDE_KB;
static int osk_y0, osk_h;             // top edge and height (logical pixels) of the keyboard; osk_h = 0 when hidden
static bool osk_new;                  // set on every keyboard touch (restarts the idle timer)
static bool osk_act;                  // keyboard was touched: schedule the quality pass
static int osk_layout(void);
static void osk_draw_all(void);
static void osk_quality(void);
static void refresh_rect(WFM_MODE_INDEX_T wfm, int lx, int ly, int w, int h, bool flash);

static bool mode_on(GhosttyMode m) {
	GhosttyTerminalModeConfig mc = {0};
	mc.mode = m;
	if (ghostty_terminal_get(term, GHOSTTY_TERMINAL_DATA_MODE, &mc) != GHOSTTY_SUCCESS) return false;
	return mc.value;
}

static void compute_geometry(void) {
	bool sideways = rotation == 90 || rotation == 270;
	lw = sideways ? scr_h : scr_w;
	lh = sideways ? scr_w : scr_h;
	cols = forced_cols ? forced_cols : lw / cell_w;
	osk_h = osk_visible ? osk_layout() : 0;
	osk_y0 = lh - osk_h;
	rows = forced_rows ? forced_rows : (lh - osk_h) / cell_h;
	if (rows < 4) rows = 4;
	if (rows >= MAXROWS) rows = MAXROWS - 1;
	grid_ox = (lw - cols * cell_w) / 2;
	grid_oy = 0;
}

static void settings_save(void) {
	if (!settings_path) return;
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.tmp", settings_path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "version=2\n");
	fprintf(f, "rotation=%d\nrotation_auto=%d\nauto_rotation=%d\npx=%d\ntouch=%d\nscroll_natural=%d\ntheme=%d\ncolors=%d\n", rot_manual >= 0 ? rot_manual : rotation, rot_auto, rot_auto_last >= 0 ? rot_auto_last : rotation, ttf_px, touch_mode, scroll_natural, theme_idx, color_mode);
	fprintf(f, "keyboard=%d\nrefresh_wfm=%d\nrefresh_batch_ms=%d\nrefresh_quality_ms=%d\nrefresh_flash_rows=%d\n", osk_mode, rf_wfm, rf_batch_ms, rf_quality_ms, rf_flash_rows);
	fprintf(f, "gesture_up=%d\ngesture_down=%d\n", gest_up, gest_down);
	if (set_bright >= 0) fprintf(f, "brightness=%d\n", set_bright);
	if (set_warm >= 0) fprintf(f, "warmth=%d\n", set_warm);
	fflush(f); fsync(fileno(f)); fclose(f);
	rename(tmp, settings_path);          // atomic: a power cut leaves the old file, never a half-written one
}

static void settings_load(bool px_cli, bool rot_cli) {
	FILE *f = fopen(settings_path, "r");
	if (!f) return;
	char l[64];
	int v;
	while (fgets(l, sizeof l, f)) {
		if (sscanf(l, "rotation=%d", &v) == 1 && !rot_cli && (v == 0 || v == 90 || v == 180 || v == 270)) rotation = v;
		else if (sscanf(l, "rotation_auto=%d", &v) == 1) rot_auto = v != 0;
		else if (sscanf(l, "auto_rotation=%d", &v) == 1 && (v == 0 || v == 90 || v == 180 || v == 270)) rot_auto_last = v;
		else if (sscanf(l, "px=%d", &v) == 1 && !px_cli && v >= 16 && v <= 64) ttf_px = v;
		else if (sscanf(l, "touch=%d", &v) == 1 && v >= 0 && v <= 2) touch_mode = v;
		else if (sscanf(l, "scroll_natural=%d", &v) == 1) scroll_natural = v != 0;
		else if (sscanf(l, "theme=%d", &v) == 1 && v >= 0 && v <= 1) theme_idx = v;
		else if (sscanf(l, "colors=%d", &v) == 1 && v >= 0 && v <= 3) color_mode = v;
		else if (sscanf(l, "gesture_up=%d", &v) == 1 && v >= 0 && v < GA_COUNT) gest_up = v;
		else if (sscanf(l, "gesture_down=%d", &v) == 1 && v >= 0 && v < GA_COUNT) gest_down = v;
		else if (sscanf(l, "keyboard=%d", &v) == 1 && v >= 0 && v <= 2) osk_mode = v;
		else if (sscanf(l, "refresh_wfm=%d", &v) == 1 && v >= 0 && v <= 2) rf_wfm = v;
		else if (sscanf(l, "refresh_batch_ms=%d", &v) == 1 && v >= 10 && v <= 2000) rf_batch_ms = v;
		else if (sscanf(l, "refresh_quality_ms=%d", &v) == 1 && v >= 0 && v <= 20000) rf_quality_ms = v;
		else if (sscanf(l, "refresh_flash_rows=%d", &v) == 1 && v >= 0 && v <= 5000) rf_flash_rows = v;
		else if (sscanf(l, "brightness=%d", &v) == 1 && v >= 0 && v <= 100) set_bright = v;
		else if (sscanf(l, "warmth=%d", &v) == 1 && v >= 0 && v <= 100) set_warm = v;
	}
	fclose(f);
	rot_manual = rotation;
	if (rot_auto && rot_auto_last >= 0 && !rot_cli) rotation = rot_auto_last;   // start where the sensor last left it
}

// Frontlight (Kobo Libra Colour / "monza": white LEDs via mxc_msp430, warmth via the lm3630a mixer, inverted).
#define FL_WHITE "/sys/class/backlight/mxc_msp430.0/brightness"
#define FL_COLOR "/sys/class/backlight/lm3630a_led/color"
static const int fl_levels[] = {0, 1, 2, 3, 5, 8, 12, 18, 25, 35, 50, 70, 100};
#define NLEV ((int)(sizeof fl_levels / sizeof *fl_levels))
static int sysfs_read(const char *pth) {
	int v = -1;
	FILE *f = fopen(pth, "r");
	if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
	return v;
}
static void sysfs_write(const char *pth, int v) {
	FILE *f = fopen(pth, "w");
	if (f) { fprintf(f, "%d\n", v); fclose(f); }
}
// Other devices (Kindle): the first /sys/class/backlight entry with brightness + max_brightness, scaled to 0-100. No warmth control.
static char fl_alt[160];
static int fl_alt_max;
static void fl_probe(void) {
	static bool done;
	if (done) return;
	done = true;
	if (sysfs_read(FL_WHITE) >= 0) return;
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char pth[200];
		snprintf(pth, sizeof pth, "/sys/class/backlight/%s/max_brightness", e->d_name);
		int mx = sysfs_read(pth);
		if (mx <= 0) continue;
		snprintf(fl_alt, sizeof fl_alt, "/sys/class/backlight/%s/brightness", e->d_name);
		fl_alt_max = mx;
		LOG("frontlight %s (max %d)", fl_alt, mx);
		break;
	}
	if (d) closedir(d);
}
static int fl_bright(void) {
	fl_probe();
	if (!fl_alt_max) return sysfs_read(FL_WHITE);
	int v = sysfs_read(fl_alt);
	return v < 0 ? -1 : (v * 100 + fl_alt_max / 2) / fl_alt_max;
}
static int fl_warm_pct(void) { fl_probe(); if (fl_alt_max) return -1; int raw = sysfs_read(FL_COLOR); return raw < 0 ? -1 : (10 - raw) * 10; }
static void fl_set_bright(int v) {
	fl_probe();
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	if (fl_alt_max) sysfs_write(fl_alt, (v * fl_alt_max + 50) / 100); else sysfs_write(FL_WHITE, v);
}
static void fl_set_warm(int pct) { fl_probe(); if (fl_alt_max) return; int raw = 10 - (pct + 5) / 10; sysfs_write(FL_COLOR, raw < 0 ? 0 : raw > 10 ? 10 : raw); }
static int fl_level_index(int v) {
	int best = 0;
	for (int i = 0; i < NLEV; i++) if (abs(fl_levels[i] - v) < abs(fl_levels[best] - v)) best = i;
	return best;
}

// ---- terminal themes (kept basic: light and dark) ----
typedef struct { const char *name; GhosttyColorRgb bg, fg; uint8_t pal[16][3]; } Theme;
static const Theme themes[] = {
	{"Light", {0xff, 0xff, 0xff}, {0, 0, 0}, {
		{0, 0, 0}, {0xa0, 0, 0}, {0, 0x80, 0}, {0x80, 0x60, 0}, {0, 0, 0xb0}, {0x90, 0, 0x90}, {0, 0x70, 0x80}, {0x60, 0x60, 0x60},
		{0x40, 0x40, 0x40}, {0xd0, 0, 0}, {0, 0xa0, 0}, {0xa0, 0x80, 0}, {0, 0x40, 0xe0}, {0xc0, 0, 0xc0}, {0, 0x90, 0xa0}, {0x20, 0x20, 0x20}}},
	{"Dark", {0, 0, 0}, {0xe8, 0xe8, 0xe8}, {
		{0x30, 0x30, 0x30}, {0xff, 0x6b, 0x6b}, {0x5f, 0xd7, 0x5f}, {0xff, 0xd7, 0x5f}, {0x6f, 0xa8, 0xff}, {0xd7, 0x87, 0xff}, {0x5f, 0xd7, 0xd7}, {0xe4, 0xe4, 0xe4},
		{0x7a, 0x7a, 0x7a}, {0xff, 0x8f, 0x8f}, {0x87, 0xff, 0x87}, {0xff, 0xe7, 0x87}, {0x8f, 0xba, 0xff}, {0xe3, 0xa8, 0xff}, {0x87, 0xff, 0xff}, {0xff, 0xff, 0xff}}},
};
#define NTHEMES ((int)(sizeof themes / sizeof *themes))

static void apply_theme(void) {
	const Theme *t = &themes[theme_idx < NTHEMES ? theme_idx : 0];
	GhosttyColorRgb bg = t->bg, fg = t->fg, palette[256];
	for (int k = 0; k < 256; k++) {
		if (k < 16) { palette[k] = (GhosttyColorRgb){t->pal[k][0], t->pal[k][1], t->pal[k][2]}; continue; }
		if (k < 232) { int v = k - 16; int r = v / 36, g = (v / 6) % 6, b = v % 6; palette[k] = (GhosttyColorRgb){r ? r * 40 + 55 : 0, g ? g * 40 + 55 : 0, b ? b * 40 + 55 : 0}; continue; }
		int l = 8 + (k - 232) * 10; palette[k] = (GhosttyColorRgb){l, l, l};
	}
	ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
	ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
	ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
	defbg = bg; deffg = fg;
}

// Redraw the whole screen anti-aliased; flash = clear ghosting (and the margins) with a flashing refresh.
static void apply_geometry(void);
static void redraw_all(bool flash) {
	if (plug_dirty && !menu_open) {      // plugin keys were switched on/off: the keyboard height changed
		plug_dirty = false;
		if (osk_visible) { apply_geometry(); return; }
	}
	if (flash) fill_fb(defbg);
	aa_pass = true;
	draw(true);
	aa_pass = false;
	memset(q_rows, 0, sizeof q_rows);
	memset(slow_row, 0, sizeof slow_row);
	q_any = false; burst_slow = 0;
	if (osk_visible) osk_draw_all();
	if (flash && osk_visible) {          // one flash for the whole screen rather than one per part
		memset(rr, 0, sizeof rr); rr_any = false;
		refresh_rect(WFM_GC16, 0, 0, lw, lh, true);
		return;
	}
	refresh_flash = flash;
	refresh_dirty(WFM_GC16, true);
	refresh_flash = false;
	if (osk_visible) refresh_rect(WFM_GC16, 0, osk_y0, lw, osk_h, false);
}

static GhosttyMouseEncoder menc;
static GhosttyMouseEvent mev;
static void mouse_update_size(void) {
	if (!menc) return;
	GhosttyMouseEncoderSize sz = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
	sz.screen_width = lw; sz.screen_height = lh;
	sz.cell_width = cell_w; sz.cell_height = cell_h;
	sz.padding_top = grid_oy; sz.padding_left = grid_ox;
	sz.padding_bottom = lh - grid_oy - rows * cell_h;
	sz.padding_right = lw - grid_ox - cols * cell_w;
	ghostty_mouse_encoder_setopt(menc, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &sz);
}

// New rotation or font size: recompute the grid, tell the terminal and the program inside, redraw.
static void apply_geometry(void) {
	ttf_metrics();
	compute_geometry();
	ghostty_terminal_resize(term, cols, rows, cell_w, cell_h);
	struct winsize ws = {rows, cols, 0, 0};
	ioctl(ptyfd, TIOCSWINSZ, &ws);
	mouse_update_size();
	memset(rr, 0, sizeof rr);
	rr_any = false;
	cur_x = cur_y = -1;
	LOG("geometry rot=%d px=%d cell %dx%d -> %dx%d", rotation, ttf_px, cell_w, cell_h, cols, rows);
	redraw_all(true);
}

// ---- plugins (see docs/plugins.md): menu entries and extra keyboard keys read from <plugins dir>/NAME.plugin/ ----
// A plugin folder may hold  menu  (lines "Label|command": typed into the terminal and run) and  osk  (lines
// "Label|keys": an extra key on the on-screen keyboard that sends the keys; ^X is Ctrl+X, \e Escape, \n Enter, \t Tab,
// \\ a backslash). bin/ and init.sh are used by the shell (shrc). Plugins are off until listed in <plugins dir>/enabled.
#include <ctype.h>
#define MAX_PMENU 12
#define MAX_MACRO 16
static const char *plugins_dir;
static struct { char label[40]; char cmd[200]; } pmenu[MAX_PMENU];
static int npmenu;
static struct { char label[16]; char seq[64]; } macro[MAX_MACRO];
static int nmacro;

static void unescape_keys(const char *in, char *out, size_t n) {
	size_t o = 0;
	for (; *in && o + 1 < n; in++) {
		if (*in == '\\' && in[1]) {
			in++;
			out[o++] = *in == 'e' ? 27 : *in == 'n' ? '\r' : *in == 't' ? '\t' : *in;
		} else if (*in == '^' && in[1]) { in++; out[o++] = (char)(toupper((unsigned char)*in) & 0x1f); }
		else out[o++] = *in;
	}
	out[o] = 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char dir[72]; char name[40]; bool enabled; } Plugin;
#define MAX_PLUGINS 12
static Plugin plugs[MAX_PLUGINS];
static int nplugs;

// Which plugins are on is kept in <plugins dir>/enabled, one plugin name (the folder name without ".plugin") per line.
// No file means every plugin is off. The shell side (shrc, plugins.sh) reads the same file.
static bool plugin_listed(const char *base) {
	char path[300], line[100];
	bool on = false;
	snprintf(path, sizeof path, "%s/enabled", plugins_dir);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strcmp(line, base)) on = true;
	}
	fclose(f);
	return on;
}

static void plugin_name(const char *dir, char *out, size_t n) {
	char path[300], line[160];
	snprintf(out, n, "%.*s", (int)(strlen(dir) - 7), dir);
	snprintf(path, sizeof path, "%s/%s/plugin.conf", plugins_dir, dir);
	FILE *f = fopen(path, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, "name=", 5)) { line[strcspn(line, "\r\n")] = 0; if (line[5]) snprintf(out, n, "%s", line + 5); }
	fclose(f);
}

// (Re)read the plugins folder: every NAME.plugin is listed; only the ones named in the enabled file add menu entries and keys.
static void plugins_load(void) {
	npmenu = nmacro = nplugs = 0;
	if (!plugins_dir) return;
	DIR *d = opendir(plugins_dir);
	if (!d) return;
	char *names[64];
	int nn = 0;
	struct dirent *e;
	while ((e = readdir(d)) && nn < 64) {
		size_t l = strlen(e->d_name);
		if (l > 7 && !strcmp(e->d_name + l - 7, ".plugin")) names[nn++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, nn, sizeof *names, cmp_str);
	for (int i = 0; i < nn; i++) {
		char base[72];
		snprintf(base, sizeof base, "%.*s", (int)(strlen(names[i]) - 7), names[i]);
		bool enabled = plugin_listed(base);
		if (nplugs < MAX_PLUGINS) {
			snprintf(plugs[nplugs].dir, sizeof plugs[0].dir, "%s", names[i]);
			plugs[nplugs].enabled = enabled;
			plugin_name(names[i], plugs[nplugs].name, sizeof plugs[0].name);
			nplugs++;
		}
		for (int kind = 0; kind < 2 && enabled; kind++) {
			char path[300], line[300];
			snprintf(path, sizeof path, "%s/%s/%s", plugins_dir, names[i], kind ? "osk" : "menu");
			FILE *f = fopen(path, "r");
			if (!f) continue;
			while (fgets(line, sizeof line, f)) {
				line[strcspn(line, "\r\n")] = 0;
				char *bar = strchr(line, '|');
				if (line[0] == '#' || !bar || bar == line) continue;
				*bar = 0;
				if (kind == 0 && npmenu < MAX_PMENU) {
					snprintf(pmenu[npmenu].label, sizeof pmenu[0].label, "%s", line);
					snprintf(pmenu[npmenu].cmd, sizeof pmenu[0].cmd, "%s", bar + 1);
					npmenu++;
				} else if (kind == 1 && nmacro < MAX_MACRO) {
					snprintf(macro[nmacro].label, sizeof macro[0].label, "%s", line);
					unescape_keys(bar + 1, macro[nmacro].seq, sizeof macro[0].seq);
					nmacro++;
				}
			}
			fclose(f);
		}
		free(names[i]);
	}
	LOG("plugins: %d found, %d menu entries, %d keyboard keys", nplugs, npmenu, nmacro);
}

// Switch plugin i on or off (rewrites the enabled file); menu entries and keyboard keys follow at once, shell parts on restart.
static void plugin_toggle(int i) {
	char path[300], tmp[310];
	if (i < 0 || i >= nplugs) return;
	plugs[i].enabled = !plugs[i].enabled;
	snprintf(path, sizeof path, "%s/enabled", plugins_dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) { LOG("plugin toggle failed: %s", strerror(errno)); plugs[i].enabled = !plugs[i].enabled; return; }
	for (int k = 0; k < nplugs; k++)
		if (plugs[k].enabled) fprintf(f, "%.*s\n", (int)(strlen(plugs[k].dir) - 7), plugs[k].dir);
	fflush(f); fsync(fileno(f)); fclose(f);
	rename(tmp, path);
	plugins_load();
	plug_dirty = true;
}

// ---- on-screen keyboard ---------------------------------------------------------------------
// Two rows of terminal keys (Esc Tab ~ - Home Up End PgUp / Ctrl Alt / | Left Down Right PgDn), optional rows of plugin
// keys, then a US layout: number row (Fn turns it into F1-F12), QWERTY, home, shift and bottom rows. Ctrl, Alt and
// Shift are sticky: one tap = the next key only, a second tap = locked, a third = off. The terminal is resized to the
// space above the keyboard whenever it is shown or hidden.
enum { K_KEY, K_SHIFT, K_CTRL, K_ALT, K_FN, K_MACRO, K_MENU };
typedef struct { const char *lo, *up; int code; int w; int kind; int fshift; int x, y, kw, kh; const char *seq; } OskKey;
#define A_UP "\x01"
#define A_DN "\x02"
#define A_LT "\x03"
#define A_RT "\x04"
static const OskKey R_BAR1[] = {{"Esc", "Esc", KEY_ESC, 10}, {"Tab", "Tab", KEY_TAB, 10}, {"~", "~", KEY_GRAVE, 10, K_KEY, 1}, {"-", "-", KEY_MINUS, 10},
	{"Home", "Home", KEY_HOME, 10}, {A_UP, A_UP, KEY_UP, 10}, {"End", "End", KEY_END, 10}, {"PgUp", "PgUp", KEY_PAGEUP, 10}};
static const OskKey R_BAR2[] = {{"Ctrl", "Ctrl", 0, 10, K_CTRL}, {"Alt", "Alt", 0, 10, K_ALT}, {"/", "/", KEY_SLASH, 10}, {"|", "|", KEY_BACKSLASH, 10, K_KEY, 1},
	{A_LT, A_LT, KEY_LEFT, 10}, {A_DN, A_DN, KEY_DOWN, 10}, {A_RT, A_RT, KEY_RIGHT, 10}, {"PgDn", "PgDn", KEY_PAGEDOWN, 10}};
static const OskKey R_NUM[] = {{"`", "~", KEY_GRAVE, 10}, {"1", "!", KEY_1, 10}, {"2", "@", KEY_2, 10}, {"3", "#", KEY_3, 10}, {"4", "$", KEY_4, 10},
	{"5", "%", KEY_5, 10}, {"6", "^", KEY_6, 10}, {"7", "&", KEY_7, 10}, {"8", "*", KEY_8, 10}, {"9", "(", KEY_9, 10}, {"0", ")", KEY_0, 10},
	{"-", "_", KEY_MINUS, 10}, {"=", "+", KEY_EQUAL, 10}, {"Bksp", "Bksp", KEY_BACKSPACE, 16}};
static const OskKey R_FN[] = {{"F1", "F1", KEY_F1, 10}, {"F2", "F2", KEY_F2, 10}, {"F3", "F3", KEY_F3, 10}, {"F4", "F4", KEY_F4, 10}, {"F5", "F5", KEY_F5, 10},
	{"F6", "F6", KEY_F6, 10}, {"F7", "F7", KEY_F7, 10}, {"F8", "F8", KEY_F8, 10}, {"F9", "F9", KEY_F9, 10}, {"F10", "F10", KEY_F10, 10},
	{"F11", "F11", KEY_F11, 10}, {"F12", "F12", KEY_F12, 10}, {"Bksp", "Bksp", KEY_BACKSPACE, 16}};
static const OskKey R_QWE[] = {{"q", "Q", KEY_Q, 10}, {"w", "W", KEY_W, 10}, {"e", "E", KEY_E, 10}, {"r", "R", KEY_R, 10}, {"t", "T", KEY_T, 10},
	{"y", "Y", KEY_Y, 10}, {"u", "U", KEY_U, 10}, {"i", "I", KEY_I, 10}, {"o", "O", KEY_O, 10}, {"p", "P", KEY_P, 10},
	{"[", "{", KEY_LEFTBRACE, 10}, {"]", "}", KEY_RIGHTBRACE, 10}, {"\\", "|", KEY_BACKSLASH, 10}};
static const OskKey R_ASD[] = {{"a", "A", KEY_A, 10}, {"s", "S", KEY_S, 10}, {"d", "D", KEY_D, 10}, {"f", "F", KEY_F, 10}, {"g", "G", KEY_G, 10},
	{"h", "H", KEY_H, 10}, {"j", "J", KEY_J, 10}, {"k", "K", KEY_K, 10}, {"l", "L", KEY_L, 10}, {";", ":", KEY_SEMICOLON, 10},
	{"'", "\"", KEY_APOSTROPHE, 10}, {"Enter", "Enter", KEY_ENTER, 18}};
static const OskKey R_ZXC[] = {{"Shift", "Shift", 0, 18, K_SHIFT}, {"z", "Z", KEY_Z, 10}, {"x", "X", KEY_X, 10}, {"c", "C", KEY_C, 10}, {"v", "V", KEY_V, 10},
	{"b", "B", KEY_B, 10}, {"n", "N", KEY_N, 10}, {"m", "M", KEY_M, 10}, {",", "<", KEY_COMMA, 10}, {".", ">", KEY_DOT, 10}, {"/", "?", KEY_SLASH, 10},
	{"Shift", "Shift", 0, 18, K_SHIFT}};
static const OskKey R_BOT[] = {{"Fn", "Fn", 0, 12, K_FN}, {"Menu", "Menu", 0, 12, K_MENU}, {"Space", "Space", KEY_SPACE, 52}, {"Del", "Del", KEY_DELETE, 12}, {"Ins", "Ins", KEY_INSERT, 12}};

#define OSK_MAXKEYS 128
static OskKey osk_keys[OSK_MAXKEYS];
static int osk_n;
static int osk_mod[3];               // shift, ctrl, alt: 0 off, 1 next key only, 2 locked
static bool osk_fn;
static OskKey *osk_down_key, *osk_rep, *osk_lit;
static double osk_lit_until, osk_lit_from;
static double osk_next_rep;

static int osk_add_row(const OskKey *row, int n, int y, int kh) {
	int tot = 0;
	for (int i = 0; i < n; i++) tot += row[i].w;
	int x = 0, acc = 0;
	for (int i = 0; i < n && osk_n < OSK_MAXKEYS; i++) {
		OskKey *k = &osk_keys[osk_n++];
		*k = row[i];
		acc += row[i].w;
		int x1 = (lw - 2) * acc / tot;      // the last frame's right border takes the final 2 px
		k->x = x; k->kw = x1 - x; k->y = y; k->kh = kh;
		x = x1;
	}
	return y + kh;
}

// Fill osk_keys with rects for the current logical screen; returns the keyboard height in pixels.
static int osk_layout(void) {
	int kh = (lw < lh ? lw : lh) / 17;
	if (kh < 40) kh = 40;
	int nmrows = (nmacro + 7) / 8;
	int nrows = 2 + nmrows + 5;
	int h = nrows * kh + 2;           // the keys' own frames are the keyboard's border (2 px shared)
	int y = lh - h;
	osk_n = 0;
	y = osk_add_row(R_BAR1, NOPTS(R_BAR1), y, kh);
	y = osk_add_row(R_BAR2, NOPTS(R_BAR2), y, kh);
	for (int m = 0; m < nmrows; m++) {
		OskKey row[8];
		int c = 0;
		for (int i = m * 8; i < nmacro && c < 8; i++)
			row[c++] = (OskKey){macro[i].label, macro[i].label, 0, 10, K_MACRO, 0, 0, 0, 0, 0, macro[i].seq};
		y = osk_add_row(row, c, y, kh);
	}
	y = osk_add_row(osk_fn ? R_FN : R_NUM, osk_fn ? NOPTS(R_FN) : NOPTS(R_NUM), y, kh);
	y = osk_add_row(R_QWE, NOPTS(R_QWE), y, kh);
	y = osk_add_row(R_ASD, NOPTS(R_ASD), y, kh);
	y = osk_add_row(R_ZXC, NOPTS(R_ZXC), y, kh);
	osk_add_row(R_BOT, NOPTS(R_BOT), y, kh);
	return h;
}

static void osk_fill(int x, int y, int w, int h, GhosttyColorRgb c) {
	uint32_t p = pack_rgb(c.r, c.g, c.b);
	for (int yy = y < 0 ? 0 : y; yy < y + h && yy < lh; yy++)
		for (int xx = x < 0 ? 0 : x; xx < x + w && xx < lw; xx++) *pix(xx, yy) = p;
}

static uint32_t utf8_next(const char **sp) {
	const unsigned char *s = (const unsigned char *)*sp;
	uint32_t c = *s++;
	int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (extra) c &= 0x3f >> extra;
	while (extra-- > 0 && (*s & 0xc0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
	*sp = (const char *)s;
	return c;
}

static stbtt_fontinfo *font_for(uint32_t cp) {
	if (stbtt_FindGlyphIndex(&ttf_fi[0], cp)) return &ttf_fi[0];
	if (have_sym_font && stbtt_FindGlyphIndex(&ttf_fi[2], cp)) return &ttf_fi[2];
	return &ttf_fi[0];
}

// Draw `s` centred in the rect, shrinking it to fit.
static void osk_text(int x, int y, int w, int h, const char *s, GhosttyColorRgb fg, GhosttyColorRgb bg, float px) {
	float width;
	for (;; px -= 2) {
		width = 0;
		const char *t = s;
		while (*t) {
			uint32_t cp = utf8_next(&t);
			stbtt_fontinfo *fi = font_for(cp);
			int adv, lsb;
			stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
			width += adv * stbtt_ScaleForPixelHeight(fi, px);
		}
		if (width <= w - 6 || px <= 14) break;
	}
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&ttf_fi[0], &asc, &desc, &gap);
	float sc0 = stbtt_ScaleForPixelHeight(&ttf_fi[0], px);
	int base = y + (int)((h - (asc - desc) * sc0) / 2 + asc * sc0 + 0.5f);
	float cx = x + (w - width) / 2;
	while (*s) {
		uint32_t cp = utf8_next(&s);
		stbtt_fontinfo *fi = font_for(cp);
		float sc = stbtt_ScaleForPixelHeight(fi, px);
		int adv, lsb, x0, y0, x1, y1;
		stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
		stbtt_GetCodepointBitmapBox(fi, cp, sc, sc, &x0, &y0, &x1, &y1);
		int gw = x1 - x0, gh = y1 - y0;
		if (gw > 0 && gh > 0 && gw < 200 && gh < 200) {
			uint8_t *bm = calloc(gw, gh);
			stbtt_MakeCodepointBitmap(fi, bm, gw, gh, gw, sc, sc, cp);
			for (int yy = 0; yy < gh; yy++)
				for (int xx = 0; xx < gw; xx++) {
					int a = gamma_lut[bm[yy * gw + xx]];
					if (!a) continue;
					int px_ = (int)cx + x0 + xx, py_ = base + y0 + yy;
					if (px_ < 0 || py_ < 0 || px_ >= lw || py_ >= lh) continue;
					*pix(px_, py_) = pack_rgb(bg.r + (fg.r - bg.r) * a / 255, bg.g + (fg.g - bg.g) * a / 255, bg.b + (fg.b - bg.b) * a / 255);
				}
			free(bm);
		}
		cx += adv * sc;
	}
}

// Filled triangle pointing up (1), down (2), left (3) or right (4), centred at cx,cy, `s` px across.
static void osk_arrow(int cx, int cy, int s, int dir, GhosttyColorRgb c) {
	uint32_t p = pack_rgb(c.r, c.g, c.b);
	for (int dy = -s; dy <= s; dy++)
		for (int dx = -s; dx <= s; dx++) {
			int u, v;
			switch (dir) {
			case 1: u = dx; v = dy; break;
			case 2: u = dx; v = -dy; break;
			case 3: u = dy; v = dx; break;
			default: u = dy; v = -dx;
			}
			if (v < -s / 2 || v > s / 2) continue;
			int half = (v + s / 2) / 2;
			if (abs(u) <= half) *pix(cx + dx, cy + dy) = p;
		}
}

static bool osk_key_active(const OskKey *k) {
	if (k->kind == K_SHIFT) return osk_mod[0] != 0;
	if (k->kind == K_CTRL) return osk_mod[1] != 0;
	if (k->kind == K_ALT) return osk_mod[2] != 0;
	if (k->kind == K_FN) return osk_fn;
	return false;
}

static void osk_draw_key(const OskKey *k, bool pressed) {
	GhosttyColorRgb F = deffg, B = defbg;
	bool inv = pressed || osk_key_active(k);
	int locked_mod = (k->kind == K_SHIFT && osk_mod[0] == 2) || (k->kind == K_CTRL && osk_mod[1] == 2) || (k->kind == K_ALT && osk_mod[2] == 2);
	// Neighbouring keys share one 2 px border: each frame reaches 2 px into the next cell.
	int x = k->x, y = k->y, w = k->kw + 2, h = k->kh + 2;
	osk_fill(x, y, w, h, F);
	osk_fill(x + 2, y + 2, w - 4, h - 4, inv ? F : B);
	if (locked_mod) {            // locked modifier: a light inner frame inside the dark key
		osk_fill(x + 5, y + 5, w - 10, h - 10, B);
		osk_fill(x + 8, y + 8, w - 16, h - 16, F);
	}
	GhosttyColorRgb fg = inv ? B : F, bg = inv ? F : B;
	const char *label = (osk_mod[0] && k->kind == K_KEY && !k->fshift) ? k->up : k->lo;
	if ((unsigned char)label[0] >= 1 && (unsigned char)label[0] <= 4) osk_arrow(x + w / 2, y + h / 2, h / 3, label[0], fg);
	else osk_text(x, y, w, h, label, fg, bg, k->kh * 0.42f);
}

static void osk_draw_all(void) {
	if (!osk_visible) return;
	osk_layout();
	glyph_binary = false;
	osk_fill(0, osk_y0, lw, osk_h, defbg);
	for (int i = 0; i < osk_n; i++) osk_draw_key(&osk_keys[i], false);
}

static void refresh_rect(WFM_MODE_INDEX_T wfm, int lx, int ly, int w, int h, bool flash) {
	FBInkConfig c = cfg;
	c.wfm_mode = wfm;
	c.is_flashing = flash;
	int px, py, pw, ph;
	phys_rect(lx, ly, w, h, &px, &py, &pw, &ph);
	if (pw > 0 && ph > 0 && fbink_refresh(fb, py, px, pw, ph, &c) < 0) LOG("refresh_rect failed");
}

static void osk_key_refresh(const OskKey *k) { refresh_rect(WFM_DU, k->x, k->y, k->kw + 2, k->kh + 2, false); }

// After typing settles the keyboard is redrawn with the quality waveform (its labels were drawn with the fast one).
static void osk_quality(void) {
	if (osk_visible && osk_act) { osk_act = false; refresh_rect(WFM_GC16, 0, osk_y0, lw, osk_h, false); }
}

static void osk_redraw_fast(void) { osk_draw_all(); refresh_rect(WFM_DU, 0, osk_y0, lw, osk_h, false); }

static void osk_reset_mods(void) {
	bool changed = false;
	for (int i = 0; i < 3; i++) if (osk_mod[i] == 1) { osk_mod[i] = 0; changed = true; }
	if (changed) osk_redraw_fast();
}

static bool session_dead;
static void reconnect_session(void);
static void scroll_to_bottom(void) {
	GhosttyTerminalScrollViewport sv = {0};
	sv.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
	ghostty_terminal_scroll_viewport(term, sv);
}

static void osk_send(const OskKey *k) {
	int s = mod_shift, c = mod_ctrl, a = mod_alt;
	if (osk_mod[0] || k->fshift) mod_shift = 1;
	if (osk_mod[1]) mod_ctrl = 1;
	if (osk_mod[2]) mod_alt = 1;
	char out[32];
	int len = encode_key(k->code, out);
	mod_shift = s; mod_ctrl = c; mod_alt = a;
	if (len > 0) { scroll_to_bottom(); pty_write(out, len); }
}

static void osk_sync(bool force);
static void osk_unlight(void);
static void menu_show(void);
static void menu_key(int code, int shift);
// Finger down on a key. Returns true when the terminal needs a redraw.
static bool osk_press(OskKey *k) {
	osk_act = osk_new = true;
	osk_unlight();
	switch (k->kind) {
	case K_SHIFT: case K_CTRL: case K_ALT: {
		int *m = &osk_mod[k->kind - K_SHIFT];
		*m = (*m + 1) % 3;
		osk_redraw_fast();
		return false;
	}
	case K_FN: osk_fn = !osk_fn; osk_redraw_fast(); return false;
	case K_MENU: menu_show(); return false;       // the only way into the menu on devices without buttons (Kindle)
	case K_MACRO: if (!session_dead && !menu_open) { scroll_to_bottom(); pty_write(k->seq, strlen(k->seq)); } break;
	default:
		if (menu_open) { menu_key(k->code, osk_mod[0]); break; }
		if (session_dead) { if (k->code == KEY_ENTER) { reconnect_session(); return true; } return false; }
		osk_send(k);
		osk_rep = k; osk_next_rep = now() + 0.5;
	}
	osk_draw_key(k, true);
	osk_key_refresh(k);
	osk_down_key = k;
	osk_lit_from = now();
	return false;
}

// A tapped key stays inverted for at least 150 ms so the flash is visible; osk_unlight() ends it.
static void osk_unlight(void) {
	if (!osk_lit) return;
	osk_draw_key(osk_lit, false);
	osk_key_refresh(osk_lit);
	osk_lit = NULL;
	osk_reset_mods();
}

static void osk_release(void) {
	osk_rep = NULL;
	if (osk_down_key) {
		osk_lit = osk_down_key;
		osk_down_key = NULL;
		if (now() >= osk_lit_from + 0.15) osk_unlight();
		else osk_lit_until = osk_lit_from + 0.15;
	}
}

static OskKey *osk_hit(int lx, int ly) {
	if (!osk_visible || ly < osk_y0) return NULL;
	for (int i = 0; i < osk_n; i++)
		if (lx >= osk_keys[i].x && lx < osk_keys[i].x + osk_keys[i].kw && ly >= osk_keys[i].y && ly < osk_keys[i].y + osk_keys[i].kh) return &osk_keys[i];
	return NULL;
}

static bool osk_want(void) {
	if (osk_mode == 1) return true;
	if (osk_mode == 2) return false;
	for (int i = 0; i < nk; i++) if (kfd[i] >= 0) return false;     // a Bluetooth keyboard is connected
	return true;
}

// Show or hide the keyboard according to the setting; resizes the terminal when that changes it.
static void osk_sync(bool force) {
	bool want = osk_want();
	if (want == osk_visible && !force) return;
	osk_visible = want;
	apply_geometry();
}

// ---- menu (pages: main, orientation, text size, frontlight, touch) ----
enum { PG_MAIN, PG_ORIENT, PG_TEXT, PG_THEME, PG_COLORS, PG_LIGHT, PG_TOUCH, PG_REFRESH, PG_KEYBOARD, PG_PLUGINS, PG_PLUGRUN };
enum { A_RESUME = 1, A_ORIENT, A_TEXT, A_THEME, A_COLORS, A_LIGHT, A_TOUCH, A_REFRESH, A_LOCK, A_REBOOT, A_POWEROFF, A_BACK, A_RFMENU, A_KBMENU, A_PLUGMENU,
       A_RFBATCH, A_RFQUAL, A_RFFLASH, A_RFRESET, A_ROTAUTO, A_GESTUP, A_GESTDOWN, A_PLUGRUN,
       A_ROT = 100, A_SIZE = 200, A_THEMEPICK = 250, A_COLORPICK = 260, A_BRIGHTER = 300, A_DIMMER, A_WARMER, A_COOLER, A_TOUCHMODE = 400, A_SCROLLDIR = 410, A_RFWFM = 420, A_OSKMODE = 430, A_PLUGIN = 500, A_PLUGTOG = 520 };
typedef struct { char label[72]; int id; } MItem;
#define MENU_ITEMS 14          /* the main page is the longest; the box keeps this height on every page */
static MItem mitems[MENU_ITEMS + 2];   /* plugin entries are capped (MAX_PMENU) so that the longest page fits */
static int mcount, menu_page, menu_sel;
static int menu_c0, menu_r0, menu_w, menu_h;
static const int text_px[] = {22, 26, 30, 34, 40, 46, 54};
static const char *text_name[] = {"Tiny", "Small", "Medium", "Large", "Larger", "Huge", "Giant"};
#define NSIZES ((int)(sizeof text_px / sizeof *text_px))

static void madd(int id, const char *fmt, ...) {
	if (mcount >= (int)(sizeof mitems / sizeof *mitems)) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(mitems[mcount].label, sizeof mitems[mcount].label, fmt, ap);
	va_end(ap);
	mitems[mcount++].id = id;
}

static int cols_for_px(int px) {
	int adv, lsb;
	stbtt_GetCodepointHMetrics(&ttf_fi[0], '0', &adv, &lsb);
	int cw = (int)(adv * stbtt_ScaleForPixelHeight(&ttf_fi[0], px) + 0.5f);
	return cw > 0 ? lw / cw : 0;
}

static void menu_build(void) {
	mcount = 0;
	static const char *orient[] = {"Portrait", "Landscape (rotated right)", "Portrait (upside down)", "Landscape (rotated left)"};
	switch (menu_page) {
	case PG_MAIN:
		madd(A_RESUME, "Resume");
		madd(A_ORIENT, "Orientation  >");
		madd(A_TEXT, "Text size  >");
		madd(A_THEME, "Theme  >");
		madd(A_COLORS, "Colors  >");
		madd(A_LIGHT, "Frontlight  >");
		madd(A_TOUCH, "Touch  >");
		madd(A_RFMENU, "Screen refresh  >");
		madd(A_KBMENU, "On-screen keyboard  >");
		madd(A_PLUGMENU, "Plugins  >");
		madd(A_REFRESH, "Clear ghosting");
		madd(A_LOCK, "Lock screen");
#ifdef GHOSTTY_INK_KINDLE
		madd(A_REBOOT, "Exit to Kindle");
#else
		madd(A_REBOOT, "Exit to Kobo (reboots)");
#endif
		madd(A_POWEROFF, "Power off");
		break;
	case PG_ORIENT:
		if (acc_dir[0]) madd(A_ROTAUTO, "Auto (follow the sensor)%s", rot_auto ? "   [now]" : "");
		for (int i = 0; i < 4; i++) madd(A_ROT + i, "%s%s", orient[i], !rot_auto && rotation == i * 90 ? "   [now]" : "");
		madd(A_BACK, "Back");
		break;
	case PG_TEXT:
		for (int i = 0; i < NSIZES; i++)
			madd(A_SIZE + i, "%-7s %4.1f pt  ~%d cols%s", text_name[i], text_px[i] * 72 / 300.0, cols_for_px(text_px[i]), text_px[i] == ttf_px ? "  [now]" : "");
		madd(A_BACK, "Back");
		break;
	case PG_THEME:
		for (int i = 0; i < NTHEMES; i++) madd(A_THEMEPICK + i, "%s%s", themes[i].name, theme_idx == i ? "   [now]" : "");
		madd(A_BACK, "Back");
		break;
	case PG_COLORS: {
		static const char *cn[] = {"Full color (as sent)", "Readable color (adds contrast)", "Grayscale", "Black & white"};
		for (int i = 0; i < 4; i++) madd(A_COLORPICK + i, "%s%s", cn[i], color_mode == i ? "   [now]" : "");
		madd(A_BACK, "Back");
		break;
	}
	case PG_LIGHT:
		madd(A_BRIGHTER, "Brighter      (now %d%%)", fl_bright());
		madd(A_DIMMER, "Dimmer");
		if (fl_warm_pct() >= 0) {
			madd(A_WARMER, "Warmer        (now %d%%)", fl_warm_pct());
			madd(A_COOLER, "Cooler");
		}
		madd(A_BACK, "Back");
		break;
	case PG_REFRESH: {
		static const char *wn[] = {"Fast (DU): crisp 1-bit text", "Fastest (A2): ghosts more", "Smooth (GL16): anti-aliased, slower"};
		for (int i = 0; i < 3; i++) madd(A_RFWFM + i, "%s%s", wn[i], rf_wfm == i ? "   [now]" : "");
		madd(A_RFBATCH, "Batch changes for %d ms  (tap: next)", rf_batch_ms);
		if (rf_quality_ms) madd(A_RFQUAL, "Clean-up after idle: %.1f s  (tap: next)", rf_quality_ms / 1000.0);
		else madd(A_RFQUAL, "Clean-up after idle: off  (tap: next)");
		if (rf_flash_rows) madd(A_RFFLASH, "Flash clean-up: ~%d rows  (tap: next)", rf_flash_rows);
		else madd(A_RFFLASH, "Flash clean-up: off  (tap: next)");
		madd(A_RFRESET, "Reset to defaults");
		madd(A_BACK, "Back");
		break;
	}
	case PG_KEYBOARD: {
		static const char *kn[] = {"Auto: show while no Bluetooth keyboard", "Always show", "Always hide"};
		for (int i = 0; i < 3; i++) madd(A_OSKMODE + i, "%s%s", kn[i], osk_mode == i ? "   [now]" : "");
		madd(A_BACK, "Back");
		break;
	}
	case PG_PLUGINS:
		for (int i = 0; i < nplugs; i++) madd(A_PLUGTOG + i, "%-24s [%s]", plugs[i].name, plugs[i].enabled ? "on" : "off");
		if (!nplugs) madd(A_BACK, "No plugins in the plugins folder");
		if (npmenu) madd(A_PLUGRUN, "Plugin commands  >");
		madd(A_BACK, "Back");
		break;
	case PG_PLUGRUN:
		for (int i = 0; i < npmenu; i++) madd(A_PLUGIN + i, "%s", pmenu[i].label);
		madd(A_BACK, "Back");
		break;
	case PG_TOUCH:
		madd(A_TOUCHMODE + 0, "Touch off%s", touch_mode == 0 ? "   [now]" : "");
		madd(A_TOUCHMODE + 1, "Scroll only%s", touch_mode == 1 ? "   [now]" : "");
		madd(A_TOUCHMODE + 2, "Scroll + mouse clicks%s", touch_mode == 2 ? "   [now]" : "");
		madd(A_SCROLLDIR, "Swipe: %s", scroll_natural ? "content follows finger" : "reversed");
		madd(A_GESTUP, "2-finger swipe up: %s", gesture_names[gest_up]);
		madd(A_GESTDOWN, "2-finger swipe down: %s", gesture_names[gest_down]);
		madd(A_BACK, "Back");
		break;
	}
	if (menu_sel >= mcount) menu_sel = mcount - 1;
}

static void status_text(char *out, size_t n) {
	int bat = -1;
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char pth[160];
		snprintf(pth, sizeof pth, "/sys/class/power_supply/%s/capacity", e->d_name);
		bat = sysfs_read(pth);
		if (bat >= 0) break;
	}
	if (d) closedir(d);
	char ip[32] = "no Wi-Fi";
	struct ifaddrs *ifa, *p;
	if (getifaddrs(&ifa) == 0) {
		for (p = ifa; p; p = p->ifa_next)
			if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strncmp(p->ifa_name, "wlan", 4))
				inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, sizeof ip);
		freeifaddrs(ifa);
	}
	int nkb = 0;
	for (int i = 0; i < nk; i++) if (kfd[i] >= 0) nkb++;
	snprintf(out, n, "Battery %d%%  %s  Keyboards: %d", bat, ip, nkb);
}

static void menu_draw(void) {
	static const char *titles[] = {"Ghostty Ink menu", "Orientation", "Text size", "Theme", "Colors", "Frontlight", "Touch", "Screen refresh", "On-screen keyboard", "Plugins (tap: on/off)", "Plugin commands"};
	GhosttyColorRgb W = {255, 255, 255}, K = {0, 0, 0};
	menu_w = 56 < cols - 2 ? 56 : cols - 2;
	menu_h = MENU_ITEMS + 5;
	menu_c0 = (cols - menu_w) / 2;
	menu_r0 = (rows - menu_h) / 2;
	if (menu_r0 < 0) menu_r0 = 0;
	glyph_binary = false;
	char line[160];
	for (int r = 0; r < menu_h && menu_r0 + r < rows; r++) {
		memset(line, ' ', sizeof line);
		bool edge = r == 0 || r == menu_h - 1;
		bool sep = r == 3;
		if (edge || sep) memset(line, '-', menu_w);
		line[0] = (edge || sep) ? '+' : '|';
		line[menu_w - 1] = line[0];
		line[menu_w] = 0;
		bool sel = false;
		char tmp[96];
		if (r == 1) { snprintf(tmp, sizeof tmp, "%s   (top: select, bottom: next)", titles[menu_page]); memcpy(line + 2, tmp, strnlen(tmp, menu_w - 4)); }
		if (r == 2) { status_text(tmp, sizeof tmp); memcpy(line + 2, tmp, strnlen(tmp, menu_w - 4)); }
		if (r >= 4 && r - 4 < mcount) {
			int i = r - 4;
			sel = i == menu_sel;
			snprintf(tmp, sizeof tmp, "%s %s", sel ? ">" : " ", mitems[i].label);
			memcpy(line + 2, tmp, strnlen(tmp, menu_w - 4));
		}
		for (int c = 0; c < menu_w; c++) {
			unsigned char ch = (unsigned char)line[c];
			bool inv = sel && c > 0 && c < menu_w - 1;
			draw_cell(menu_c0 + c, menu_r0 + r, ch ? ch : ' ', inv ? W : K, inv ? K : W, false, 0, false);
		}
	}
	refresh_rows(WFM_GC16, menu_r0, menu_r0 + menu_h - 1 < rows ? menu_r0 + menu_h - 1 : rows - 1);
}

static void menu_goto(int page) {
	menu_page = page;
	menu_sel = 0;
	menu_build();
	// preselect the current choice on chooser pages
	for (int i = 0; i < mcount; i++) if (strstr(mitems[i].label, "[now]")) menu_sel = i;
	menu_draw();
}
static void menu_show(void) { menu_open = true; menu_goto(PG_MAIN); }
static void menu_close(void) { menu_open = false; redraw_all(false); }
static void menu_next(void) { menu_sel = (menu_sel + 1) % mcount; menu_draw(); }
static void menu_prev(void) { menu_sel = (menu_sel + mcount - 1) % mcount; menu_draw(); }
static void menu_back(void) { if (menu_page == PG_MAIN) menu_close(); else menu_goto(PG_MAIN); }

static void lock_screen(void);
static void acc_enable(bool on);
static void menu_activate(void) {
	int id = mitems[menu_sel].id;
	if (id >= A_ROT && id < A_ROT + 4) { rotation = rot_manual = (id - A_ROT) * 90; if (rot_auto) acc_enable(false); rot_auto = 0; rot_pending = -1; settings_save(); menu_open = false; apply_geometry(); return; }
	if (id >= A_SIZE && id < A_SIZE + NSIZES) { ttf_px = text_px[id - A_SIZE]; settings_save(); menu_open = false; apply_geometry(); return; }
	if (id >= A_THEMEPICK && id < A_THEMEPICK + NTHEMES) { theme_idx = id - A_THEMEPICK; settings_save(); menu_open = false; apply_theme(); redraw_all(true); return; }
	if (id >= A_COLORPICK && id < A_COLORPICK + 4) { color_mode = id - A_COLORPICK; settings_save(); menu_open = false; redraw_all(true); return; }
	if (id >= A_TOUCHMODE && id < A_TOUCHMODE + 3) { touch_mode = id - A_TOUCHMODE; settings_save(); menu_build(); menu_draw(); return; }
	if (id >= A_RFWFM && id < A_RFWFM + 3) { rf_wfm = id - A_RFWFM; settings_save(); menu_build(); menu_draw(); return; }
	if (id >= A_OSKMODE && id < A_OSKMODE + 3) {
		osk_mode = id - A_OSKMODE; settings_save(); menu_open = false;
		if (osk_want() != osk_visible) osk_sync(false); else redraw_all(false);
		return;
	}
	if (id >= A_PLUGTOG && id < A_PLUGTOG + nplugs) {
		int keep = menu_sel;
		plugin_toggle(id - A_PLUGTOG);
		menu_build(); menu_sel = keep < mcount ? keep : mcount - 1; menu_draw();
		return;
	}
	if (id >= A_PLUGIN && id < A_PLUGIN + npmenu) {
		menu_open = false;
		redraw_all(false);
		if (!session_dead) { char cmd[210]; int n = snprintf(cmd, sizeof cmd, "%s\r", pmenu[id - A_PLUGIN].cmd); scroll_to_bottom(); pty_write(cmd, n); }
		return;
	}
	switch (id) {
	case A_ROTAUTO: rot_auto = 1; acc_enable(true); rot_pending = -1; if (rot_auto_last >= 0) rotation = rot_auto_last; settings_save(); menu_open = false; apply_geometry(); break;
	case A_RFMENU: menu_goto(PG_REFRESH); break;
	case A_KBMENU: menu_goto(PG_KEYBOARD); break;
	case A_PLUGMENU: menu_goto(PG_PLUGINS); break;
	case A_PLUGRUN: menu_goto(PG_PLUGRUN); break;
	case A_GESTUP: gest_up = (gest_up + 1) % GA_COUNT; settings_save(); menu_build(); menu_draw(); break;
	case A_GESTDOWN: gest_down = (gest_down + 1) % GA_COUNT; settings_save(); menu_build(); menu_draw(); break;
	case A_RFBATCH: case A_RFQUAL: case A_RFFLASH: {
		int *v = id == A_RFBATCH ? &rf_batch_ms : id == A_RFQUAL ? &rf_quality_ms : &rf_flash_rows;
		const int *o = id == A_RFBATCH ? rf_batch_opts : id == A_RFQUAL ? rf_quality_opts : rf_flash_opts;
		int n = id == A_RFBATCH ? NOPTS(rf_batch_opts) : id == A_RFQUAL ? NOPTS(rf_quality_opts) : NOPTS(rf_flash_opts), i;
		for (i = 0; i < n; i++) if (o[i] == *v) break;
		*v = o[(i + 1) % n];
		settings_save(); menu_build(); menu_draw();
		break;
	}
	case A_RFRESET: rf_wfm = 0; rf_batch_ms = 60; rf_quality_ms = 600; rf_flash_rows = 160; settings_save(); menu_build(); menu_draw(); break;
	case A_RESUME: menu_close(); break;
	case A_ORIENT: menu_goto(PG_ORIENT); break;
	case A_TEXT: menu_goto(PG_TEXT); break;
	case A_THEME: menu_goto(PG_THEME); break;
	case A_COLORS: menu_goto(PG_COLORS); break;
	case A_LIGHT: menu_goto(PG_LIGHT); break;
	case A_TOUCH: menu_goto(PG_TOUCH); break;
	case A_BACK: menu_goto(PG_MAIN); break;
	case A_REFRESH: menu_open = false; redraw_all(true); break;
	case A_LOCK: menu_open = false; lock_screen(); break;
	case A_REBOOT: exit_code = 0; menu_open = false; quit = 1; break;
	case A_POWEROFF: exit_code = 42; menu_open = false; quit = 1; break;
	case A_BRIGHTER: case A_DIMMER: {
		int i = fl_level_index(fl_bright()) + (id == A_BRIGHTER ? 1 : -1);
		i = i < 0 ? 0 : i >= NLEV ? NLEV - 1 : i;
		fl_set_bright(fl_levels[i]); set_bright = fl_levels[i]; settings_save();
		menu_build(); menu_draw();
		break;
	}
	case A_WARMER: case A_COOLER: {
		int w = fl_warm_pct() + (id == A_WARMER ? 10 : -10);
		w = w < 0 ? 0 : w > 100 ? 100 : w;
		fl_set_warm(w); set_warm = w; settings_save();
		menu_build(); menu_draw();
		break;
	}
	case A_SCROLLDIR: scroll_natural = !scroll_natural; settings_save(); menu_build(); menu_draw(); break;
	}
}

// A key pressed while the menu is open (Bluetooth keyboard or the on-screen one) navigates it.
static void menu_key(int code, int shift) {
	LOG("menu key %d (page %d, item %d/%d)", code, menu_page, menu_sel, mcount);
	if (code == KEY_UP || code == KEY_K || (code == KEY_TAB && shift)) menu_prev();
	else if (code == KEY_DOWN || code == KEY_J || code == KEY_TAB) menu_next();
	else if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_RIGHT || code == KEY_SPACE) menu_activate();
	else if (code == KEY_ESC || code == KEY_Q || code == KEY_LEFT || code == KEY_BACKSPACE) menu_back();
	else if (code >= KEY_1 && code <= KEY_9 && code - KEY_1 < mcount) { menu_sel = code - KEY_1; menu_activate(); }
}

// ---- lock screen (power button): blank, frontlight off, all input ignored until the power button is pressed again ----
static int saved_bright = -1;
static void lock_screen(void) {
	locked = true;
	menu_open = false;
	saved_bright = fl_bright();
	fl_set_bright(0);
	GhosttyColorRgb W = {255, 255, 255}, K = {0, 0, 0};
	fill_fb(W);
	glyph_binary = false;
	static const char *l1 = "Locked", *l2 = "press the power button to unlock";
	int r = rows / 2;
	for (int i = 0; l1[i]; i++) draw_cell((cols - (int)strlen(l1)) / 2 + i, r - 1, (unsigned char)l1[i], K, W, true, 0, false);
	for (int i = 0; l2[i]; i++) draw_cell((cols - (int)strlen(l2)) / 2 + i, r + 1, (unsigned char)l2[i], K, W, false, 0, false);
	refresh_flash = true;
	refresh_rows(WFM_GC16, 0, rows - 1);
	refresh_flash = false;
}
static void unlock_screen(void) {
	locked = false;
	if (saved_bright >= 0) fl_set_bright(saved_bright);
	redraw_all(true);
}

// ---- touch: taps/long-press become mouse clicks, drags become wheel events / arrow keys / scrollback ----
static int t_slot, t_track = -1, t_rx, t_ry, t_down, t_moved, t_sx, t_sy, t_lx, t_ly, t_lasty;
static double t_t0;
static int t_logged;
static int s1_track = -1, s1_rx, s1_ry;    // second finger (slot 1)
static bool g_active, g_fired, t_gesture;   // two fingers down / gesture already triggered / this touch became a gesture
static int g0x, g0y, g1x, g1y;              // where the two fingers started
static bool t_in_osk;                 // the current touch started on the on-screen keyboard

static void phys_to_logical(int x, int y, int *lx, int *ly) {
	switch (rotation) {
	case 90: *lx = y; *ly = scr_w - 1 - x; break;
	case 180: *lx = scr_w - 1 - x; *ly = scr_h - 1 - y; break;
	case 270: *lx = scr_h - 1 - y; *ly = x; break;
	default: *lx = x; *ly = y;
	}
}

static void touch_to_logical(int rx, int ry, int *lx, int *ly) {
	double fx = rx / (double)tmaxx, fy = ry / (double)tmaxy;
	if (fst.touch_swap_axes) { double t = fx; fx = fy; fy = t; }
	if (fst.touch_mirror_x) fx = 1.0 - fx;
	if (fst.touch_mirror_y) fy = 1.0 - fy;
	phys_to_logical((int)(fx * (scr_w - 1)), (int)(fy * (scr_h - 1)), lx, ly);
}

static bool mouse_tracking(void) {
	return mode_on(GHOSTTY_MODE_X10_MOUSE) || mode_on(GHOSTTY_MODE_NORMAL_MOUSE) || mode_on(GHOSTTY_MODE_BUTTON_MOUSE) || mode_on(GHOSTTY_MODE_ANY_MOUSE);
}

static void send_mouse(int action, int button, int lx, int ly) {
	if (!menc || !mev) return;
	ghostty_mouse_encoder_setopt_from_terminal(menc, term);
	ghostty_mouse_event_set_action(mev, action);
	ghostty_mouse_event_set_button(mev, button);
	ghostty_mouse_event_set_position(mev, (GhosttyMousePosition){(float)lx, (float)ly});
	char out[64];
	size_t n = 0;
	if (ghostty_mouse_encoder_encode(menc, mev, out, sizeof out, &n) == GHOSTTY_SUCCESS && n) pty_write(out, n);
}

// One scroll step: up = look at older output / wheel up.
static bool scroll_step(bool up, int lx, int ly) {
	if (touch_mode >= 1 && mouse_tracking()) {
		send_mouse(GHOSTTY_MOUSE_ACTION_PRESS, up ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE, lx, ly);
		return false;
	}
	if (mode_on(GHOSTTY_MODE_ALT_SCREEN_SAVE) || mode_on(GHOSTTY_MODE_ALT_SCREEN) || mode_on(GHOSTTY_MODE_ALT_SCREEN_LEGACY)) {
		char out[16];
		int len = encode_key(up ? KEY_UP : KEY_DOWN, out);
		if (len > 0) pty_write(out, len);
		return false;
	}
	GhosttyTerminalScrollViewport sv = {0};
	sv.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
	sv.value.delta = up ? -1 : 1;
	ghostty_terminal_scroll_viewport(term, sv);
	return true;
}

static void gesture_do(int a) {
	switch (a) {
	case GA_SHOW_KB: osk_mode = 1; settings_save(); osk_sync(false); break;
	case GA_HIDE_KB: osk_mode = 2; settings_save(); osk_sync(false); break;
	case GA_CLEAR: redraw_all(true); break;
	case GA_MENU: menu_show(); break;
	}
}

// ---- edge sliders: one finger sliding up/down along the left edge changes the frontlight, along the right edge the
// warmth (only where the device has it). A temporary overlay shows the value; it goes away shortly after the finger lifts.
static int t_edge;                     // this touch started on an edge: 1 left (brightness), 2 right (warmth)
static bool t_edge_moved;
static int edge_y0, edge_v0, edge_v;   // where the slide started, the value then, the value now
static bool ov_up;                     // overlay on screen
static double ov_until, ov_drawn;

static int edge_width(void) { int w = lw / 14; return w < 50 ? 50 : w; }

static void overlay_draw(const char *what, int v) {
	if (!osk_visible && !cell_h) return;
	char buf[40];
	snprintf(buf, sizeof buf, "%s %d%%", what, v);
	int bw = lw / 2, bh = cell_h * 2, x = (lw - bw) / 2, y = lh / 3;
	glyph_binary = false;
	osk_fill(x, y, bw, bh, deffg);
	osk_fill(x + 3, y + 3, bw - 6, bh - 6, defbg);
	osk_text(x + 3, y + 3, bw - 6, bh - 6, buf, deffg, defbg, cell_h * 1.1f);
	refresh_rect(WFM_DU, x, y, bw, bh, false);
	ov_up = true;
	ov_drawn = now();
}

static void edge_start(int lx, int ly) {
	t_edge = 0;
	if (menu_open || locked) return;
	int ew = edge_width();
	if (lx < ew && fl_bright() >= 0) t_edge = 1;
	else if (lx >= lw - ew && fl_warm_pct() >= 0) t_edge = 2;
	if (!t_edge) return;
	t_edge_moved = false;
	edge_y0 = ly;
	LOG("edge slide %s starts (rotation %d, raw %d,%d -> logical %d,%d)", t_edge == 1 ? "left" : "right", rotation, t_rx, t_ry, lx, ly);
	edge_v0 = edge_v = t_edge == 1 ? fl_bright() : fl_warm_pct();
}

static void edge_move(int ly) {
	int span = lh / 2;
	int v = edge_v0 + (edge_y0 - ly) * 100 / span;
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	if (v == edge_v && t_edge_moved) return;
	t_edge_moved = true;
	edge_v = v;
	if (t_edge == 1) fl_set_bright(v); else fl_set_warm(v);
	if (now() - ov_drawn >= 0.12) overlay_draw(t_edge == 1 ? "Frontlight" : "Warmth", v);
	ov_until = now() + 1.2;
}

static void edge_end(void) {
	if (t_edge_moved) {
		overlay_draw(t_edge == 1 ? "Frontlight" : "Warmth", edge_v);
		if (t_edge == 1) set_bright = edge_v; else set_warm = edge_v;
		settings_save();
		ov_until = now() + 1.2;
	}
	t_edge = 0;
}

static void acc_enable(bool on) {
	if (!acc_dir[0]) return;
	char pth[140];
	snprintf(pth, sizeof pth, "%s/enable", acc_dir);
	FILE *f = fopen(pth, "w");
	if (f) { fprintf(f, "%d\n", on ? 1 : 0); fclose(f); }
}

// Sample gravity ~4 times a second while auto is on. Portrait: gravity along -y; upside down: +y; device turned
// clockwise (its top edge to the right): -x; counter-clockwise: +x. (Measured on a Libra Colour, 2026-10-13.) Face-up or
// diagonal poses change nothing; a new orientation is applied once it has been stable for 0.7 s.
static void acc_poll(double t) {
	static double last;
	if (!rot_auto || !acc_dir[0] || t - last < 0.25) return;
	last = t;
	char pth[140];
	snprintf(pth, sizeof pth, "%s/xyz", acc_dir);
	FILE *f = fopen(pth, "r");
	if (!f) return;
	int x, y, z;
	int n = fscanf(f, "x:%d , y:%d , z:%d", &x, &y, &z);
	fclose(f);
	if (n != 3) return;
	int r = -1, ax = abs(x), ay = abs(y);
	if (ay > 6000 && ay * 10 > ax * 14) r = y < 0 ? 0 : 180;
	else if (ax > 6000 && ax * 10 > ay * 14) r = x < 0 ? 270 : 90;
	if (r < 0) return;
	if (r == rotation) rot_pending = -1;
	else if (r != rot_pending) { rot_pending = r; rot_pend_t = t; LOG("accelerometer x%d y%d z%d -> rotation %d", x, y, z, r); }
}

static bool touch_release(double t);
// Feed one raw input event; returns true when the terminal view needs a redraw.
static bool touch_event(const struct input_event *ev_in, double t) {
	bool redraw = false;
	if (locked) return false;
	struct input_event mapped = *ev_in;
	const struct input_event *ev = &mapped;
	if (t_single) {          // present a single-touch panel as multi-touch slot 0
		if (mapped.type == EV_KEY && mapped.code == BTN_TOUCH) { mapped.type = EV_ABS; mapped.code = ABS_MT_TRACKING_ID; mapped.value = mapped.value ? 1 : -1; }
		else if (mapped.type == EV_ABS && mapped.code == ABS_X) mapped.code = ABS_MT_POSITION_X;
		else if (mapped.type == EV_ABS && mapped.code == ABS_Y) mapped.code = ABS_MT_POSITION_Y;
	}
	if (ev->type == EV_ABS) {
		if (ev->code == ABS_MT_SLOT) t_slot = ev->value;
		else if (t_slot == 1) {
			if (ev->code == ABS_MT_TRACKING_ID) s1_track = ev->value >= 0 ? ev->value : -1;
			else if (ev->code == ABS_MT_POSITION_X) s1_rx = ev->value;
			else if (ev->code == ABS_MT_POSITION_Y) s1_ry = ev->value;
		}
		else if (t_slot != 0) return false;
		else if (ev->code == ABS_MT_TRACKING_ID) {
			if (ev->value >= 0 && t_track < 0) { t_track = ev->value; t_down = 1; t_moved = 0; t_t0 = t; t_sx = -1; }
			else if (ev->value < 0 && t_track >= 0) { t_track = -1; t_down = 2; }
		}
		else if (ev->code == ABS_MT_POSITION_X) t_rx = ev->value;
		else if (ev->code == ABS_MT_POSITION_Y) t_ry = ev->value;
	} else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
		int lx, ly;
		touch_to_logical(t_rx, t_ry, &lx, &ly);
		if (t_track >= 0 && s1_track >= 0) {
			int l1x, l1y;
			touch_to_logical(s1_rx, s1_ry, &l1x, &l1y);
			if (!g_active) {
				g_active = t_gesture = true; g_fired = false;
				g0x = lx; g0y = ly; g1x = l1x; g1y = l1y;
				if (t_in_osk) { osk_release(); t_in_osk = false; }
				t_edge = 0;
			} else if (!g_fired && !menu_open) {
				int d0 = ly - g0y, d1 = l1y - g1y, dx = ((lx - g0x) + (l1x - g1x)) / 2, dy = (d0 + d1) / 2;
				if (abs(d0) >= 100 && abs(d1) >= 100 && (d0 > 0) == (d1 > 0) && abs(dy) > abs(dx)) {
					g_fired = true;
					LOG("two-finger swipe %s (rotation %d, logical dy %d, raw %d,%d -> %d,%d)", dy < 0 ? "up" : "down", rotation, dy, t_rx, t_ry, lx, ly);
					gesture_do(dy < 0 ? gest_up : gest_down);
				}
			}
		} else g_active = false;
		if (t_down == 1) {
			t_sx = lx; t_sy = ly; t_lasty = ly; t_down = 0;
			OskKey *ok = osk_hit(lx, ly);
			t_in_osk = ok != NULL;
			if (ok) redraw |= osk_press(ok); else edge_start(lx, ly);
			if (t_logged < 30) { t_logged++; LOG("touch down raw %d,%d -> logical %d,%d (cell %d,%d)", t_rx, t_ry, lx, ly, (lx - grid_ox) / cell_w, (ly - grid_oy) / cell_h); }
		} else if (t_down == 2) {
			t_down = 0;
			bool slid = t_edge && t_edge_moved;
			if (t_edge) edge_end();
			if (t_in_osk) { osk_release(); t_in_osk = false; }
			else if (slid) { /* the slide was the whole touch */ }
			else if (t_sx >= 0 && !t_gesture) redraw = touch_release(t);
			t_sx = -1;
		} else if (t_track >= 0 && t_sx >= 0) {
			t_lx = lx; t_ly = ly;
			if (!t_moved && (abs(lx - t_sx) > 14 || abs(ly - t_sy) > 14)) t_moved = 1;
			if (t_edge && !t_gesture) { if (abs(ly - t_sy) > 14 || t_edge_moved) edge_move(ly); }
			else if (t_moved && !menu_open && !t_in_osk && !t_gesture) {
				int dy = ly - t_lasty;
				while (abs(dy) >= cell_h) {
					bool finger_down = dy > 0;
					redraw |= scroll_step(finger_down == (bool)scroll_natural, lx, ly);
					t_lasty += finger_down ? cell_h : -cell_h;
					dy = ly - t_lasty;
				}
			}
		}
		t_lx = lx; t_ly = ly;
		if (t_track < 0 && s1_track < 0) t_gesture = false;
	}
	return redraw;
}

static bool touch_release(double t) {
	if (locked || (touch_mode == 0 && !menu_open)) return false;
	int col = (t_lx - grid_ox) / cell_w, row = (t_ly - grid_oy) / cell_h;
	if (menu_open) {
		if (t_moved) return false;
		if (col >= menu_c0 && col < menu_c0 + menu_w && row >= menu_r0 && row < menu_r0 + menu_h) {
			int i = row - (menu_r0 + 4);
			if (i >= 0 && i < mcount) { menu_sel = i; menu_activate(); }
		} else menu_back();
		return false;
	}
	if (!t_moved && touch_mode == 2 && mouse_tracking()) {
		int button = (t - t_t0 >= 0.7) ? GHOSTTY_MOUSE_BUTTON_RIGHT : GHOSTTY_MOUSE_BUTTON_LEFT;
		send_mouse(GHOSTTY_MOUSE_ACTION_PRESS, button, t_lx, t_ly);
		send_mouse(GHOSTTY_MOUSE_ACTION_RELEASE, button, t_lx, t_ly);
	}
	return false;
}

// ---- page-turn buttons: top = menu (select inside it), bottom = full refresh (next item inside the menu) ----
// Returns true if the terminal screen needs a redraw.
static bool handle_button(int code, int value, double t) {
	(void)t;
	LOG("button code=%d value=%d", code, value);
	if (locked) return false;
	if (code == btn_exit) {
		if (value == 1) { if (menu_open) menu_activate(); else menu_show(); }
		return false;
	}
	if (code != btn_bottom || value != 1) return false;
	if (menu_open) menu_next();
	else redraw_all(true);     // clear ghosting: flashing full redraw
	return false;
}

// ---- pty ---------------------------------------------------------------------------------
static int spawn(char **argv) {
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) || unlockpt(m)) return -1;
	char *sn = ptsname(m);
	struct winsize ws = {rows, cols, 0, 0};
	ioctl(m, TIOCSWINSZ, &ws);
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) {
		setsid();
		int s = open(sn, O_RDWR);
		ioctl(s, TIOCSCTTY, 0);
		ioctl(s, TIOCSWINSZ, &ws);
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		if (s > 2) close(s);
		close(m);
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		char b[16];
		snprintf(b, sizeof b, "%d", cols); setenv("COLUMNS", b, 1);
		snprintf(b, sizeof b, "%d", rows); setenv("LINES", b, 1);
		execvp(argv[0], argv);
		fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	child = p;
	return m;
}

static char **g_cmd;
static double session_t0;

// The program in the pty ended (ssh dropped, shell exited...): keep the screen and say why, instead of leaving at once.
static void end_session(int status) {
	if (ptyfd >= 0) { close(ptyfd); ptyfd = -1; }
	char msg[160];
	if (WIFEXITED(status)) snprintf(msg, sizeof msg, "\r\n\033[0m[session ended: exit status %d after %.0fs. Enter = reconnect, top button = menu]\r\n", WEXITSTATUS(status), now() - session_t0);
	else if (WIFSIGNALED(status)) snprintf(msg, sizeof msg, "\r\n\033[0m[session killed by signal %d after %.0fs. Enter = reconnect, top button = menu]\r\n", WTERMSIG(status), now() - session_t0);
	else snprintf(msg, sizeof msg, "\r\n\033[0m[session ended. Enter = reconnect, top button = menu]\r\n");
	LOG("session ended status=0x%x after %.1fs", status, now() - session_t0);
	ghostty_terminal_vt_write(term, (const uint8_t *)msg, strlen(msg));
	child = -1;
	session_dead = true;
}

static void reconnect_session(void) {
	const char *msg = "\r\n[reconnecting...]\r\n";
	ghostty_terminal_vt_write(term, (const uint8_t *)msg, strlen(msg));
	ptyfd = spawn(g_cmd);
	if (ptyfd < 0) { msg = "[reconnect failed: could not start the program]\r\n"; ghostty_terminal_vt_write(term, (const uint8_t *)msg, strlen(msg)); session_dead = true; return; }
	fcntl(ptyfd, F_SETFL, O_NONBLOCK);
	session_t0 = now();
	session_dead = false;
	LOG("session (re)started");
}

#include <ucontext.h>
static void on_segv(int sig, siginfo_t *si, void *uc_) {
#ifdef __arm__
	ucontext_t *uc = uc_;
	LOG("SIGNAL %d addr=%p pc=%lx lr=%lx sp=%lx", sig, si->si_addr, (unsigned long)uc->uc_mcontext.arm_pc,
	    (unsigned long)uc->uc_mcontext.arm_lr, (unsigned long)uc->uc_mcontext.arm_sp);
#else
	(void)uc_;
	LOG("SIGNAL %d addr=%p", sig, si->si_addr);
#endif
	_exit(139);
}
static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv) {
	int font = UNSCII_TALL, mult = 2;
	bool px_cli = false, rot_cli = false;
	int opt, i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { i++; break; }
		if (!strcmp(argv[i], "-F") && i + 1 < argc) font = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-M") && i + 1 < argc) mult = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-K") && i + 1 < argc) kbd_match = argv[++i];
		else if (!strcmp(argv[i], "-C") && i + 1 < argc) forced_cols = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-R") && i + 1 < argc) forced_rows = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-L") && i + 1 < argc) logf_ = fopen(argv[++i], "w");
		else if (!strcmp(argv[i], "-I") && i + 1 < argc) inject_path = argv[++i];
		else if (!strcmp(argv[i], "-f") && i + 1 < argc) font_reg_path = argv[++i];
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) font_bold_path = argv[++i];
		else if (!strcmp(argv[i], "-S") && i + 1 < argc) { ttf_px = atoi(argv[++i]); px_cli = true; }
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) font_sym_path = argv[++i];
		else if (!strcmp(argv[i], "-E") && i + 1 < argc) font_emoji_path = argv[++i];
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) { rotation = atoi(argv[++i]) % 360; rot_cli = true; }
		else if (!strcmp(argv[i], "-P") && i + 1 < argc) settings_path = argv[++i];
		else if (!strcmp(argv[i], "-X") && i + 1 < argc) plugins_dir = argv[++i];
		else if (!strcmp(argv[i], "-T") && i + 1 < argc) btn_exit = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-D") && i + 1 < argc) btn_bottom = atoi(argv[++i]);
		else break;
	}
	(void)opt;
	if (settings_path) settings_load(px_cli, rot_cli);
	plugins_load();
	char *defcmd[] = {"/bin/sh", NULL};
	char **cmd = i < argc ? &argv[i] : defcmd;

	{ static char altstk[65536]; stack_t ss = {altstk, 0, sizeof altstk}; sigaltstack(&ss, NULL); struct sigaction sa = {0}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); }
	signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig); signal(SIGPIPE, SIG_IGN);

	cfg.is_quiet = true;
	cfg.fontname = font;
	cfg.fontmult = mult;
	cfg.no_viewport = false;
	cfg.wfm_mode = WFM_A2;
	LOG("start");
	fb = fbink_open();
	if (fb < 0 || fbink_init(fb, &cfg) < 0) { fprintf(stderr, "fbink init failed\n"); return 1; }
	LOG("fbink init ok");
	fbink_get_state(&cfg, &fst);
	cols = forced_cols ? forced_cols : fst.max_cols;
	rows = forced_rows ? forced_rows : fst.max_rows;
	int cellpx_w = fst.font_w, cellpx_h = fst.font_h;
	if (font_reg_path) {
		struct fb_var_screeninfo vi;
		struct fb_fix_screeninfo fi;
		fbink_get_fb_info(&vi, &fi);
		fbmem = fbink_get_fb_pointer(fb, &fbmem_size);
		scr_w = vi.xres; scr_h = vi.yres; fb_stride = fi.line_length;
		lw = scr_w; lh = scr_h;
		uint32_t pr, pg, pb;
		fbink_pack_pixel_rgba(255, 0, 0, 255, &pr);
		fbink_pack_pixel_rgba(0, 255, 0, 255, &pg);
		fbink_pack_pixel_rgba(0, 0, 255, 255, &pb);
		for (int sft = 0; sft < 32; sft += 8) {
			if (((pr >> sft) & 0xff) == 255 && ((pg >> sft) & 0xff) == 0 && ((pb >> sft) & 0xff) == 0) sh_r = sft;
			if (((pg >> sft) & 0xff) == 255 && ((pr >> sft) & 0xff) == 0 && ((pb >> sft) & 0xff) == 0) sh_g = sft;
			if (((pb >> sft) & 0xff) == 255 && ((pr >> sft) & 0xff) == 0 && ((pg >> sft) & 0xff) == 0) sh_b = sft;
		}
		sh_a = 24 - 0;
		for (int sft = 0; sft < 32; sft += 8) if (sft != sh_r && sft != sh_g && sft != sh_b) sh_a = sft;
		LOG("fb ptr=%p size=%zu stride=%d %dx%d bpp=%u shifts r%d g%d b%d a%d", fbmem, fbmem_size, fb_stride, scr_w, scr_h, vi.bits_per_pixel, sh_r, sh_g, sh_b, sh_a);
		if (fbmem && vi.bits_per_pixel == 32 && ttf_init()) {
			use_ttf = true;
			scan_keyboards();
			if (rot_auto) acc_enable(true);
			osk_visible = osk_want();
			cellpx_w = cell_w; cellpx_h = cell_h;
			compute_geometry();
		} else LOG("TTF renderer unavailable, using FBInk bitmap font");
	}
	fst.font_w = cellpx_w; fst.font_h = cellpx_h;
	LOG("fb %dx%d cell %dx%d -> %dx%d ttf=%d", fst.screen_width, fst.screen_height, cellpx_w, cellpx_h, cols, rows, use_ttf);

	LOG("terminal_new...");
	if (ghostty_terminal_new(NULL, &term, cols, rows) != GHOSTTY_SUCCESS) return 1;
	LOG("terminal_new done");
	ghostty_terminal_resize(term, cols, rows, fst.font_w, fst.font_h);
	if (use_ttf) { ghostty_mouse_encoder_new(NULL, &menc); ghostty_mouse_event_new(NULL, &mev); mouse_update_size(); }
	if (set_bright >= 0) fl_set_bright(set_bright);
	if (set_warm >= 0) fl_set_warm(set_warm);
	LOG("resized");
	ghostty_terminal_set(term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)on_write_pty);
	apply_theme();
	LOG("terminal ok");
	ghostty_render_state_new(NULL, &rs);
	ghostty_render_state_row_iterator_new(NULL, &rowit);
	ghostty_render_state_row_cells_new(NULL, &rowcells);

	// Clear the screen to white with a flashing GC16, then run.
	{
		FBInkConfig c = cfg;
		c.is_cleared = true; c.is_flashing = true; c.wfm_mode = WFM_GC16;
		fbink_cls(fb, &c, NULL, false);
	}
	set_pen(deffg, defbg);

	LOG("render objects ok");
	g_cmd = cmd;
	session_t0 = now();
	ptyfd = spawn(cmd);
	if (ptyfd < 0) { fprintf(stderr, "spawn failed\n"); return 1; }
	fcntl(ptyfd, F_SETFL, O_NONBLOCK);
	if (inject_path) { unlink(inject_path); if (mkfifo(inject_path, 0666) == 0) ifd = open(inject_path, O_RDWR | O_NONBLOCK); }

	double lastscan = 0, last_activity = now(), last_full = 0, next_repeat = 0, osk_pend = 0;
	int repeat_code = -1;
	bool idle_pending = false, a2_pending = false, full_pending = false;
	double last_a2 = 0;
	int rows_since_flash = 0;
	if (use_ttf) redraw_all(true);
	else { aa_pass = true; draw(true); aa_pass = false; refresh_dirty(WFM_GC16, true); }

	while (!quit) {
		double t = now();
		if (t - lastscan > 1.0) { scan_keyboards(); lastscan = t; }
		struct pollfd p[MAXDEV + 5];
		int np = 0;
		p[np++] = (struct pollfd){ptyfd, ptyfd >= 0 ? POLLIN : 0, 0};
		for (i = 0; i < nk; i++) p[np++] = (struct pollfd){kfd[i], kfd[i] >= 0 ? POLLIN : 0, 0};
		int bidx = np;
		p[np++] = (struct pollfd){bfd, bfd >= 0 ? POLLIN : 0, 0};
		int iidx = np;
		p[np++] = (struct pollfd){ifd, ifd >= 0 ? POLLIN : 0, 0};
		int tidx = np;
		p[np++] = (struct pollfd){tfd, tfd >= 0 ? POLLIN : 0, 0};
		int pidx = np;
		p[np++] = (struct pollfd){pfd, pfd >= 0 ? POLLIN : 0, 0};
		int timeout = 250;
		if (repeat_code >= 0) { int d = (int)((next_repeat - t) * 1000); timeout = d < 0 ? 0 : d; }
		else if (idle_pending) timeout = 100;
		if (a2_pending) { int d = (int)((last_a2 + rf_batch_ms / 1000.0 - t) * 1000) + 1; if (d < 0) d = 0; if (d < timeout) timeout = d; }
		if (osk_rep && timeout > 30) timeout = 30;
		if (osk_lit && timeout > 20) timeout = 20;
		if (ov_up && timeout > 100) timeout = 100;
		if ((rot_pending >= 0 || (rot_auto && acc_dir[0])) && timeout > 100) timeout = 100;
		poll(p, np, timeout);
		t = now();

		// On-screen keyboard: follow the setting (Auto = shown while no keyboard is connected; wait 3 s before flipping so
		// a keyboard that is reconnecting does not make the terminal jump about).
		if (use_ttf && !locked && !menu_open) {
			if (osk_want() == osk_visible) osk_pend = 0;
			else if (osk_pend == 0) osk_pend = t;
			else if (t - osk_pend >= 3.0) { osk_pend = 0; osk_sync(false); }
		}

		bool output = false;
		if (bfd >= 0 && (p[bidx].revents & (POLLIN | POLLERR | POLLHUP))) {
			struct input_event ev[16];
			ssize_t n = read(bfd, ev, sizeof ev);
			if (n < 0 && errno != EAGAIN) { close(bfd); bfd = -1; }
			for (int k = 0; k < n / (int)sizeof *ev; k++)
				if (ev[k].type == EV_KEY && handle_button(ev[k].code, ev[k].value, t)) output = true;
		}
		acc_poll(t);
		if (rot_pending >= 0 && !locked && t - rot_pend_t >= 0.7) {
			rotation = rot_auto_last = rot_pending;
			rot_pending = -1;
			settings_save();
			menu_open = false;
			apply_geometry();
		}
		if (pfd >= 0 && (p[pidx].revents & (POLLIN | POLLERR | POLLHUP))) {
			struct input_event ev[16];
			ssize_t n = read(pfd, ev, sizeof ev);
			if (n < 0 && errno != EAGAIN) { close(pfd); pfd = -1; }
			for (int k = 0; k < n / (int)sizeof *ev; k++) {
				if (ev[k].type != EV_KEY) continue;
				LOG("power key code=%d value=%d", ev[k].code, ev[k].value);
				if (ev[k].code == KEY_POWER && ev[k].value == 1) { if (locked) unlock_screen(); else lock_screen(); }
			}
		}
		if (tfd >= 0 && (p[tidx].revents & (POLLIN | POLLERR | POLLHUP))) {
			struct input_event ev[64];
			ssize_t n = read(tfd, ev, sizeof ev);
			if (n < 0 && errno != EAGAIN) { close(tfd); tfd = -1; }
			for (int k = 0; k < n / (int)sizeof *ev; k++)
				if (touch_event(&ev[k], t)) output = true;
		}
		if (ifd >= 0 && (p[iidx].revents & POLLIN)) {
			char ib[512];
			ssize_t n = read(ifd, ib, sizeof ib);
			if (n > 0) { if (session_dead) { if (memchr(ib, '\r', n)) { reconnect_session(); output = true; } } else pty_write(ib, n); last_activity = t; }
		}
		bool eof = false;
		if (ptyfd >= 0 && (p[0].revents & (POLLIN | POLLHUP))) {
			uint8_t buf[8192];
			for (;;) {
				ssize_t n = read(ptyfd, buf, sizeof buf);
				if (n > 0) { ghostty_terminal_vt_write(term, buf, n); output = true; if (n < (ssize_t)sizeof buf) break; continue; }
				if (n < 0 && errno == EAGAIN) break;
				eof = true; break;      // EOF/EIO: child gone (reaped below)
			}
		}
		for (i = 0; i < nk; i++) {
			if (!(p[1 + i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
			struct input_event ev[32];
			ssize_t n = read(kfd[i], ev, sizeof ev);
			if (n < 0) { if (errno != EAGAIN) { ioctl(kfd[i], EVIOCGRAB, 0); close(kfd[i]); kfd[i] = -1; LOG("lost keyboard %s", kpath[i]); } continue; }
			for (int k = 0; k < n / (int)sizeof *ev; k++) {
				if (ev[k].type != EV_KEY) continue;
				int code = ev[k].code, v = ev[k].value;
				if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { mod_shift = v != 0; continue; }
				if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) { mod_ctrl = v != 0; continue; }
				if (code == KEY_LEFTALT || code == KEY_RIGHTALT) { mod_alt = v != 0; continue; }
				if (v == 0) { if (code == repeat_code) repeat_code = -1; continue; }
				if (locked) continue;
				if (menu_open) {
					menu_key(code, mod_shift);
					continue;
				}
				// Local controls: Ctrl+Alt+Backspace quits; Shift+PgUp/PgDn scroll history.
				if (mod_ctrl && mod_alt && code == KEY_BACKSPACE) { quit = 1; continue; }
				if (mod_shift && (code == KEY_PAGEUP || code == KEY_PAGEDOWN)) {
					GhosttyTerminalScrollViewport sv = {0};
					sv.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
					sv.value.delta = (code == KEY_PAGEUP ? -1 : 1) * (rows / 2);
					ghostty_terminal_scroll_viewport(term, sv);
					output = true;
					continue;
				}
				if (session_dead) { if (code == KEY_ENTER || code == KEY_KPENTER) { reconnect_session(); output = true; } continue; }
				char out[32];
				int len = encode_key(code, out);
				if (len > 0) {
					GhosttyTerminalScrollViewport sv = {0};
					sv.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
					ghostty_terminal_scroll_viewport(term, sv);
					pty_write(out, len);
					if (v == 1) { repeat_code = code; next_repeat = t + 0.4; }
					last_activity = t;
				}
			}
		}
		if (osk_rep && osk_down_key == osk_rep && t >= osk_next_rep && !menu_open && !locked) {
			osk_send(osk_rep);
			osk_next_rep = t + 0.09;
		}
		if (osk_lit && t >= osk_lit_until) osk_unlight();
		if (ov_up && !t_edge && t >= ov_until) { ov_up = false; redraw_all(false); }
		if (osk_new) { osk_new = false; idle_pending = true; last_activity = now(); }
		if (repeat_code >= 0 && t >= next_repeat && !menu_open) {
			char out[32];
			int len = encode_key(repeat_code, out);
			if (len > 0) pty_write(out, len);
			next_repeat = t + 0.045;
		}
		if (output && !menu_open && !locked) {
			int n = draw(false);
			if (n) { a2_pending = true; idle_pending = true; last_activity = now(); rows_since_flash += n; }
		}
		if (a2_pending && now() - last_a2 >= rf_batch_ms / 1000.0) {
			refresh_dirty(typing_wfm(), false);
			last_a2 = now(); a2_pending = false;
		}
		if (idle_pending && !a2_pending && !menu_open && !locked && now() - last_activity > (rf_quality_ms ? rf_quality_ms : 600) / 1000.0) {
			if (rf_quality_ms) quality_pass();
			osk_quality();
			idle_pending = false; full_pending = true;
		}
		if (full_pending && !idle_pending && !menu_open && !locked && now() - last_activity > 4.0) {
			// Long idle after plenty of changes: one flashing whole-screen GC16 to clear accumulated ghosting.
			if (rf_flash_rows && rows_since_flash >= rf_flash_rows) {
				refresh_flash = true;
				refresh_dirty(WFM_GC16, true);
				refresh_flash = false;
				rows_since_flash = 0;
			}
			full_pending = false; last_full = now();
		}
		if (!session_dead && child > 0) {
			int st = 0;
			pid_t w = waitpid(child, &st, WNOHANG);
			if (w == child || (eof && w < 0)) {
				uint8_t buf[8192]; ssize_t n;
				while (ptyfd >= 0 && (n = read(ptyfd, buf, sizeof buf)) > 0) ghostty_terminal_vt_write(term, buf, n);
				end_session(st);
				if (!menu_open && !locked) { aa_pass = true; draw(false); aa_pass = false; refresh_dirty(WFM_GC16, true); }
			} else if (eof) {
				usleep(100000);     // EOF but not reapable yet: look again next round
			}
		}
	}
	(void)last_full;

	if (child > 0) { kill(child, SIGHUP); usleep(200000); waitpid(child, NULL, WNOHANG); }
	aa_pass = true; draw(false); refresh_dirty(WFM_GC16, true);
	for (i = 0; i < nk; i++) if (kfd[i] >= 0) { ioctl(kfd[i], EVIOCGRAB, 0); close(kfd[i]); }
	if (bfd >= 0) { ioctl(bfd, EVIOCGRAB, 0); close(bfd); }
	if (tfd >= 0) { ioctl(tfd, EVIOCGRAB, 0); close(tfd); }
	if (pfd >= 0) { ioctl(pfd, EVIOCGRAB, 0); close(pfd); }
	if (rot_auto) acc_enable(false);
	{
		FBInkConfig c = cfg;
		c.is_cleared = true; c.is_flashing = true; c.wfm_mode = WFM_GC16;
		fbink_cls(fb, &c, NULL, false);
	}
	fbink_close(fb);
	return exit_code;
}
