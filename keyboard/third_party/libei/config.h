// Client-only GNU/Linux configuration, matching the pinned upstream options.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define EI_VERSION "1.6.0"
#define EIS_VERSION "1.6.0"
#define HAVE_MEMFD_CREATE 1
#define HAVE_LIBXKBCOMMON 0
#define HAVE_LIBEVDEV 0
#define HAVE_LIBSYSTEMD 0
#define HAVE_LIBELOGIND 0
#define HAVE_BASU 0
