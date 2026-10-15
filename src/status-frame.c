#include "status-frame.h"

#include <util/platform.h>

#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STATUS_WIDTH 1280
#define STATUS_HEIGHT 720

static wchar_t *utf8_to_wide(const char *text)
{
	if (!text)
		text = "";

	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
				      -1, NULL, 0);
	if (length <= 0)
		length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (length <= 0)
		return NULL;

	wchar_t *wide = bzalloc((size_t)length * sizeof(wchar_t));
	if (!wide)
		return NULL;

	if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length)) {
		bfree(wide);
		return NULL;
	}
	return wide;
}

static HFONT create_ui_font(int pixel_height, int weight)
{
	return CreateFontW(-pixel_height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
			   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			   DEFAULT_PITCH | FF_DONTCARE,
			   L"Microsoft YaHei UI");
}

static void draw_utf8(HDC dc, const char *text, RECT rect,
		      int pixel_height, int weight, COLORREF color,
		      UINT format)
{
	wchar_t *wide = utf8_to_wide(text);
	HFONT font = create_ui_font(pixel_height, weight);
	if (!wide || !font) {
		bfree(wide);
		if (font)
			DeleteObject(font);
		return;
	}

	HGDIOBJ previous = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, color);
	DrawTextW(dc, wide, -1, &rect, format | DT_NOPREFIX);
	SelectObject(dc, previous);
	DeleteObject(font);
	bfree(wide);
}

static COLORREF status_color(enum airplay_status_tone tone)
{
	switch (tone) {
	case AIRPLAY_STATUS_TONE_READY:
		return RGB(67, 211, 126);
	case AIRPLAY_STATUS_TONE_CONNECTED:
		return RGB(83, 169, 255);
	case AIRPLAY_STATUS_TONE_ERROR:
		return RGB(255, 101, 111);
	case AIRPLAY_STATUS_TONE_STARTING:
	default:
		return RGB(241, 185, 75);
	}
}

static void fill_background(uint8_t *pixels, uint32_t stride)
{
	for (uint32_t y = 0; y < STATUS_HEIGHT; ++y) {
		uint8_t *row = pixels + (size_t)y * stride;
		for (uint32_t x = 0; x < STATUS_WIDTH; ++x) {
			/* A restrained blue-violet gradient keeps the idle frame
			 * recognizable without competing with a live production. */
			unsigned glow_x = x > 960 ? x - 960 : 960 - x;
			unsigned glow_y = y > 120 ? y - 120 : 120 - y;
			unsigned glow = glow_x / 20 + glow_y / 14;
			if (glow > 28)
				glow = 28;
			uint8_t r = (uint8_t)(19 + (28 - glow) / 3);
			uint8_t g = (uint8_t)(23 + (28 - glow) / 2);
			uint8_t b = (uint8_t)(36 + (28 - glow));
			row[x * 4 + 0] = b;
			row[x * 4 + 1] = g;
			row[x * 4 + 2] = r;
			row[x * 4 + 3] = 255;
		}
	}
}

static void draw_round_rect(HDC dc, RECT rect, int radius,
			    COLORREF fill, COLORREF outline, int outline_width)
{
	HBRUSH brush = CreateSolidBrush(fill);
	HPEN pen = CreatePen(PS_SOLID, outline_width, outline);
	HGDIOBJ old_brush = SelectObject(dc, brush);
	HGDIOBJ old_pen = SelectObject(dc, pen);
	RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
		  radius, radius);
	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	DeleteObject(pen);
	DeleteObject(brush);
}

static void draw_receiver_icon(HDC dc, COLORREF accent)
{
	RECT phone = {222, 245, 350, 480};
	draw_round_rect(dc, phone, 28, RGB(24, 29, 44), accent, 5);

	RECT screen = {237, 278, 335, 437};
	draw_round_rect(dc, screen, 12, RGB(13, 17, 28), RGB(49, 59, 82), 2);

	HBRUSH accent_brush = CreateSolidBrush(accent);
	HGDIOBJ old_brush = SelectObject(dc, accent_brush);
	HPEN null_pen = CreatePen(PS_NULL, 0, accent);
	HGDIOBJ old_pen = SelectObject(dc, null_pen);
	Ellipse(dc, 275, 327, 297, 349);
	Ellipse(dc, 301, 327, 323, 349);
	RECT bridge = {284, 334, 314, 342};
	FillRect(dc, &bridge, accent_brush);
	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	DeleteObject(null_pen);
	DeleteObject(accent_brush);

	HBRUSH muted = CreateSolidBrush(RGB(92, 105, 137));
	RECT speaker = {267, 261, 305, 265};
	RECT home = {270, 458, 302, 462};
	FillRect(dc, &speaker, muted);
	FillRect(dc, &home, muted);
	DeleteObject(muted);
}

bool airplay_status_frame_output(obs_source_t *source,
				 enum airplay_status_tone tone,
				 const char *badge, const char *heading,
				 const char *detail,
				 const char *receiver_label,
				 const char *receiver_name,
				 const char *footer)
{
	if (!source)
		return false;

	BITMAPINFO bitmap_info;
	memset(&bitmap_info, 0, sizeof(bitmap_info));
	bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info.bmiHeader.biWidth = STATUS_WIDTH;
	bitmap_info.bmiHeader.biHeight = -STATUS_HEIGHT;
	bitmap_info.bmiHeader.biPlanes = 1;
	bitmap_info.bmiHeader.biBitCount = 32;
	bitmap_info.bmiHeader.biCompression = BI_RGB;

	void *dib_pixels = NULL;
	HBITMAP bitmap = CreateDIBSection(NULL, &bitmap_info, DIB_RGB_COLORS,
					   &dib_pixels, NULL, 0);
	HDC dc = CreateCompatibleDC(NULL);
	if (!bitmap || !dc || !dib_pixels) {
		blog(LOG_ERROR, "[AirPlay] Unable to create the OBS status frame");
		if (dc)
			DeleteDC(dc);
		if (bitmap)
			DeleteObject(bitmap);
		return false;
	}

	HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);
	fill_background(dib_pixels, STATUS_WIDTH * 4);

	RECT card = {154, 92, 1126, 620};
	draw_round_rect(dc, card, 34, RGB(24, 29, 44), RGB(47, 56, 79), 2);

	COLORREF accent = status_color(tone);
	HBRUSH accent_brush = CreateSolidBrush(accent);
	HGDIOBJ old_brush = SelectObject(dc, accent_brush);
	HPEN null_pen = CreatePen(PS_NULL, 0, accent);
	HGDIOBJ old_pen = SelectObject(dc, null_pen);
	Ellipse(dc, 214, 143, 234, 163);
	SelectObject(dc, old_pen);
	SelectObject(dc, old_brush);
	DeleteObject(null_pen);
	DeleteObject(accent_brush);

	RECT badge_rect = {250, 127, 1034, 178};
	draw_utf8(dc, badge, badge_rect, 22, FW_SEMIBOLD,
		  RGB(176, 187, 214), DT_LEFT | DT_SINGLELINE | DT_VCENTER |
					 DT_END_ELLIPSIS);

	draw_receiver_icon(dc, accent);

	RECT heading_rect = {402, 223, 1035, 296};
	draw_utf8(dc, heading, heading_rect, 43, FW_SEMIBOLD,
		  RGB(245, 247, 252), DT_LEFT | DT_SINGLELINE | DT_VCENTER |
					 DT_END_ELLIPSIS);

	RECT detail_rect = {404, 303, 1033, 382};
	draw_utf8(dc, detail, detail_rect, 23, FW_NORMAL,
		  RGB(170, 181, 207), DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);

	RECT receiver_box = {400, 404, 1038, 504};
	draw_round_rect(dc, receiver_box, 18, RGB(16, 20, 32),
			RGB(43, 52, 74), 2);
	RECT receiver_label_rect = {430, 421, 1008, 450};
	draw_utf8(dc, receiver_label, receiver_label_rect, 18, FW_NORMAL,
		  RGB(130, 144, 176), DT_LEFT | DT_SINGLELINE |
					 DT_VCENTER | DT_END_ELLIPSIS);
	RECT receiver_name_rect = {430, 450, 1008, 491};
	draw_utf8(dc, receiver_name, receiver_name_rect, 27, FW_SEMIBOLD,
		  RGB(235, 240, 252), DT_LEFT | DT_SINGLELINE |
					 DT_VCENTER | DT_END_ELLIPSIS);

	RECT footer_rect = {190, 551, 1090, 594};
	draw_utf8(dc, footer, footer_rect, 19, FW_NORMAL,
		  RGB(116, 129, 158), DT_CENTER | DT_SINGLELINE |
					 DT_VCENTER | DT_END_ELLIPSIS);

	/* GDI may batch drawing commands. Flush before directly reading or
	 * modifying DIBSection bits and before libobs copies the frame. */
	GdiFlush();

	/* GDI does not preserve the alpha byte for every operation. The status
	 * slate is intentionally opaque, so normalize it after all drawing. */
	uint8_t *pixels = dib_pixels;
	for (size_t i = 0; i < (size_t)STATUS_WIDTH * STATUS_HEIGHT; ++i)
		pixels[i * 4 + 3] = 255;

	struct obs_source_frame2 frame;
	memset(&frame, 0, sizeof(frame));
	frame.data[0] = pixels;
	frame.linesize[0] = STATUS_WIDTH * 4;
	frame.width = STATUS_WIDTH;
	frame.height = STATUS_HEIGHT;
	frame.timestamp = os_gettime_ns();
	frame.format = VIDEO_FORMAT_BGRX;
	frame.range = VIDEO_RANGE_FULL;
	obs_source_output_video2(source, &frame);

	SelectObject(dc, previous_bitmap);
	DeleteDC(dc);
	DeleteObject(bitmap);
	return true;
}
