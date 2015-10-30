/*******************************************************************************
*  cchunks - Copy chunks from an input file, with flexible ranges description
*  Copyright (C) 2015 Avi Halachmi
*
*  This program is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License
*  as published by the Free Software Foundation; either version 2
*  of the License, or (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program; if not, write to the Free Software
*  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*******************************************************************************/

// Windows fugliness.
// For unicode support (enabled by default): link with shell32.lib.
//   E.g. with msvc add: shell32.lib, with gcc/tcc add: -lshell32
// To disable unicode file names, add (msvc) /DCC_DISABLE_WIN_UTF8

// MSVC - suppress warnings which we don't need:
// - fopen doesn't check for null values, we do.
#define _CRT_SECURE_NO_WARNINGS

// mingw - support %lld in printf without warning. This adds a small bulk of
// ansi formatting support to the exe and makes sure it always works.
#define __USE_MINGW_ANSI_STDIO 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// Some windows compilers (mingw) can support off_t, ftello, etc, but they
// still have to map those to the actual windows API, so use this API
// directly - less things to go wrong (64) and support other compilers too.
#include <io.h>
#include <fcntl.h>
#define cc_off_t     __int64
#define cc_fseek     _fseeki64
// _ftelli64 is hard to link. _telli64 + _fileno is the same, easier to link
#define cc_ftell(fd) _telli64(_fileno(fd))

#ifndef OFF_T_MIN
    #define OFF_T_MIN _I64_MIN
#endif
#ifndef OFF_T_MAX
    #define OFF_T_MAX _I64_MAX
#endif

// On non-gcc compilers (tcc, msvc), by default, embed a local getopt copy
#if !defined(CC_GETOPT_LOCAL) && !defined(CC_GETOPT_GLOBAL) && !defined(__GNUC__)
    // if HAVE_STRING_H then it includes string.h, otherwise strings.h which is bad
    #define HAVE_STRING_H 1

    // gnu getopt declares an incorrect prototype of getenv if it's not defined.
    // so define it to use a correct getenv
    #ifndef getenv
        #define CC_REMOVE_GETENV
        char *fugly_getenv(const char* str) {
            return getenv(str);
        }
        #define getenv fugly_getenv
    #endif

    #include "gnu-getopt/getopt.c"

    #ifdef CC_REMOVE_GETENV
        #undef getenv
    #endif

    #define CC_GETOPT_HANDLED
#endif


#ifdef CC_DISABLE_WIN_UTF8
  #define cc_fopen    fopen

#else
  #define CC_HAVE_WIN_UTF8

  #include <windows.h>
  #include <shellapi.h>

// utf8 code from mpv: https://github.com/mpv-player/mpv/blob/master/osdep/io.c#L98

/*
 * unicode/utf-8 I/O helpers and wrappers for Windows
 *
 * Contains parts based on libav code (http://libav.org).
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

wchar_t *mp_from_utf8(const char *s)
{
    int count = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (count <= 0)
        return NULL;
    wchar_t *ret = malloc(sizeof(wchar_t) * (count + 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ret, count);
    return ret;
}

char *mp_to_utf8(const wchar_t *s)
{
    int count = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (count <= 0)
        return NULL;
    char *ret = malloc(sizeof(char) * count);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, ret, count, NULL, NULL);
    return ret;
}
// ------------------- end of mpv code --------------------------------

// on success, returns a utf8 argv and sets out_success to 1. On failure returns
// the original argv. caller needs to free once done - using free_argvutf8
char **win_utf8_argv(int argc_validation, char **argv_orig, int *out_success)
{
    *out_success = 0;
    int nArgs;
    LPWSTR *szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (!szArglist || nArgs != argc_validation) {
        if (szArglist)
            LocalFree(szArglist);
        return argv_orig;
    }

    char **argvu = malloc(sizeof(char*) * (nArgs + 1));
    int i = 0;
    for (; i < nArgs; i++) {
        argvu[i] = mp_to_utf8(szArglist[i]);
        if (!argvu[i])
            return argv_orig; // leaking the previous strings. we don't care.
    }
    argvu[nArgs] = NULL;

    LocalFree(szArglist);
    *out_success = 1;
    return argvu;
}

void free_argvutf8(int argc, char** argvu)
{
    for (int i = 0; i < argc; i++)
        free(argvu[i]);
    free(argvu);
}

int g_win_utf8_enabled = 0;
FILE *cc_fopen(const char *fname, const char *mode) {
    if (g_win_utf8_enabled) {
        wchar_t *wfname = mp_from_utf8(fname);
        wchar_t *wmode = mp_from_utf8(mode);

        FILE *rv = _wfopen(wfname, wmode);

        free(wmode);
        free(wfname);
        return rv;
    }

    // implicit else
    return fopen(fname, mode);
}
#endif
