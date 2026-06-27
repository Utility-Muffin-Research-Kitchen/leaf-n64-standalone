#ifndef EMU_OVERLAY_RENDER_H
#define EMU_OVERLAY_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#define EMU_OVL_FONT_LARGE 0
#define EMU_OVL_FONT_MEDIUM 1
#define EMU_OVL_FONT_SMALL 2
#define EMU_OVL_FONT_TINY 3
#define EMU_OVL_FONT_COUNT 4

// Colors (ARGB), matched to Jawaka's default in-game menu theme.
#define EMU_OVL_COLOR_WHITE 0xFFFFFFFF
#define EMU_OVL_COLOR_GRAY 0xFF8F7F7F
#define EMU_OVL_COLOR_BLACK 0xFF000000
#define EMU_OVL_COLOR_PANEL 0xC8D3D0CC
#define EMU_OVL_COLOR_PANEL_STRONG 0xE6D3D0CC
#define EMU_OVL_COLOR_TEXT 0xFF993B41
#define EMU_OVL_COLOR_HINT 0xFF993B41
#define EMU_OVL_COLOR_HIGHLIGHT 0xFF993B41
#define EMU_OVL_COLOR_HIGHLIGHT_TEXT 0xFFD3D0CC
#define EMU_OVL_COLOR_PREVIEW_BG 0xB0000000

// Compatibility aliases used by older overlay drawing helpers.
#define EMU_OVL_COLOR_ACCENT EMU_OVL_COLOR_HIGHLIGHT
#define EMU_OVL_COLOR_BAR_BG EMU_OVL_COLOR_PANEL
#define EMU_OVL_COLOR_PILL_DARK EMU_OVL_COLOR_PANEL
#define EMU_OVL_COLOR_PILL_LIGHT EMU_OVL_COLOR_PANEL_STRONG
#define EMU_OVL_COLOR_SELECTED_BG EMU_OVL_COLOR_HIGHLIGHT
#define EMU_OVL_COLOR_LABEL_BG EMU_OVL_COLOR_PANEL_STRONG
#define EMU_OVL_COLOR_ROW_BG EMU_OVL_COLOR_PANEL
#define EMU_OVL_COLOR_ROW_SEL EMU_OVL_COLOR_HIGHLIGHT
#define EMU_OVL_COLOR_TEXT_SEL EMU_OVL_COLOR_HIGHLIGHT_TEXT
#define EMU_OVL_COLOR_TEXT_NORM EMU_OVL_COLOR_TEXT

typedef struct EmuOvlRenderBackend {
	int (*init)(int screen_w, int screen_h);
	void (*destroy)(void);
	void (*draw_rect)(int x, int y, int w, int h, uint32_t color);
	// Anti-aliased rounded rectangle. If radius < 0, defaults to h/2 (pill shape).
	void (*draw_rounded_rect)(int x, int y, int w, int h, int radius, uint32_t color);
	void (*draw_text)(const char* text, int x, int y, uint32_t color, int font_id);
	int (*text_width)(const char* text, int font_id);
	int (*text_height)(int font_id);
	void (*begin_frame)(void);
	void (*end_frame)(void);
	void (*capture_frame)(void);
	void (*draw_captured_frame)(float dim);
	// Icon support (BMP files for screenshots, embedded ARGB data for button hints)
	int (*load_icon)(const char* path, int target_height); // returns icon_id (>=0) or -1
	int (*load_icon_rgba)(const uint32_t* pixels, int w, int h, int target_height);
	void (*free_icon)(int icon_id);
	void (*draw_icon)(int icon_id, int x, int y);
	int (*icon_width)(int icon_id);
	int (*icon_height)(int icon_id);
	// Save captured frame as BMP
	int (*save_captured_frame)(const char* path);
} EmuOvlRenderBackend;

#endif
