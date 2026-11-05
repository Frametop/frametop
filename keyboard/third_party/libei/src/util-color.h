/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* ANSI color codes for terminal colors */

#pragma once

#include "config.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define COLOR_SET \
X(RESET,		"\x1B[0m")	\
X(BLACK,		"\x1B[0;30m")   \
X(RED,			"\x1B[0;31m")   \
X(GREEN,		"\x1B[0;32m")   \
X(YELLOW,		"\x1B[0;33m")   \
X(BLUE,			"\x1B[0;34m")   \
X(MAGENTA,		"\x1B[0;35m")   \
X(CYAN,			"\x1B[0;36m")   \
X(WHITE,		"\x1B[0;37m")   \
X(BRIGHT_RED,		"\x1B[0;31;1m") \
X(BRIGHT_GREEN,		"\x1B[0;32;1m") \
X(BRIGHT_YELLOW,	"\x1B[0;33;1m") \
X(BRIGHT_BLUE,		"\x1B[0;34;1m") \
X(BRIGHT_MAGENTA,	"\x1B[0;35;1m") \
X(BRIGHT_CYAN,		"\x1B[0;36;1m") \
X(BRIGHT_WHITE,		"\x1B[0;37;1m")   \
X(HIGHLIGHT,		"\x1B[0;1;39m")

/**
 * Expands into
 * enum Color {
 *     RESET,
 *     RED,
 *     ...
 * }
 */
enum AnsiColor {
#define X(name, code) name,
	COLOR_SET
#undef X
};

/**
 * Expands into an array usable via: colorcode[RED], etc.
 */
static const char *const ansi_colorcode[] __attribute__((unused)) = {
#define X(name, code) code,
	COLOR_SET
#undef X
};

/* ANSI escape sequence for true RBB colors */
#define ANSI_FG_RGB(r, g, b)	"\x1B[38;2;" #r ";" #g ";" #b "m"
#define ANSI_BG_RGB(r, g, b)	"\x1B[48;2;" #r ";" #g ";" #b "m"

#define RGB(r_, g_, b_) ((r_ << 16) | (g_ << 8) | b_)
#define RGB_BG(r_, g_, b_) (((r_ << 16) | (g_ << 8) | b_) << 32)
static inline uint64_t
rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return (r << 16) | (g << 8) | b;
}

static inline uint64_t
rgb_bg(uint8_t r, uint8_t g, uint8_t b)
{
	return (((uint64_t)r << 16) | ((uint64_t)g << 8) | (uint64_t)b) << 32;
}

static inline bool __attribute__((format(printf, 3, 0)))
cvdprintf(int fd, uint64_t rgb, const char *format, va_list args)
{
	char buf[1024];
	int rc = vsnprintf(buf, sizeof(buf), format, args);
	if (rc < 0 || (size_t)rc >= sizeof(buf))
		return false;

	if (!isatty(fd)) {
		dprintf(fd, "%s", buf);
	} else {
		uint32_t fg = rgb & 0xffffff;
		uint32_t bg = (rgb >> 32) & 0xffffff;

		uint8_t r = (fg & 0xff0000) >> 16, g = (fg & 0x00ff00) >> 8, b = (fg & 0x0000ff);
		uint8_t bgr = (bg & 0xff0000) >> 16, bgg = (bg & 0x00ff00) >> 8,
			bgb = (bg & 0x0000ff);
		const char *color_fg = "", *color_bg = "";
		const char *reset = ansi_colorcode[RESET];

		char fgstr[64];
		char bgstr[64];
		if (fg) {
			snprintf(fgstr, sizeof(fgstr), "\x1B[38;2;%u;%u;%um", r, g, b);
			color_fg = fgstr;
		}
		if (bg) {
			snprintf(bgstr, sizeof(bgstr), "\x1B[48;2;%u;%u;%um", bgr, bgg, bgb);
			color_bg = bgstr;
		}

		dprintf(fd, "%s%s%s%s", color_bg, color_fg, buf, reset);
	}
	return true;
}

static inline void __attribute__((format(printf, 3, 4)))
cdprintf(int fd, uint64_t rgb, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	cvdprintf(fd, rgb, format, args);
	va_end(args);
}

static inline void __attribute__((format(printf, 2, 3)))
cprintf(uint64_t rgb, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	cvdprintf(STDOUT_FILENO, rgb, format, args);
	va_end(args);
}

static inline void __attribute__((format(printf, 3, 4)))
cfprintf(FILE *fp, uint64_t rgb, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	cvdprintf(fileno(fp), rgb, format, args);
	va_end(args);
}
