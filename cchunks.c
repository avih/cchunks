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
*
*  Build: $CC cchunks.c -o cchunks
*  Tested $CC as: gcc (win/osx/linux), clang (osx/linux), tcc (win), cl (msvc - win)
*******************************************************************************/


// Use these if your compiler supports 64b off_t but doesn't use it by default.
// Not required on Windows even if the compiler supports them (mingw)
// #define _FILE_OFFSET_BITS 64
// #define _LARGEFILE_SOURCE
// #define _LARGEFILE64_SOURCE

#ifdef _WIN32
    #include "win_compat.h"
#else
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <limits.h>
    #include <stdint.h>
    #include <sys/types.h>

    #define cc_off_t    off_t
    #define cc_fseek    fseeko
    #define cc_ftell    ftello
    #define cc_fopen    fopen
    #define cc_fprintf  fprintf

    /**************************************************************************/
    // From: http://src.chromium.org/native_client/trunk/src/native_client/src/include/portability.h

    /*
     * Copyright (c) 2012 The Native Client Authors. All rights reserved.
     * Use of this source code is governed by a BSD-style license that can be
     * found in the LICENSE file.
     */

    /* use uint64_t as largest integral type, assume 8 bit bytes */
    #ifndef OFF_T_MIN
    # define OFF_T_MIN ((off_t) (((uint64_t) 1) << (8 * sizeof(off_t) - 1)))
    #endif
    #ifndef OFF_T_MAX
    # define OFF_T_MAX ((off_t) ~(((uint64_t) 1) << (8 * sizeof(off_t) - 1)))
    #endif
    /**************************************************************************/
#endif

#ifndef CC_GETOPT_HANDLED
    #ifndef CC_GETOPT_LOCAL
        #include <getopt.h>
    #else
        // To use a local getopt copy: cc cchunks.c gnu-getopt/getopt.c -DCC_GETOPT_LOCAL
        // You might need to add -DHAVE_STRING_H
        #include "gnu-getopt/getopt.h"
    #endif
#endif


#define CCVERSION "0.4.1"
#define RW_BUFFSIZE (512 * 1024)

// -p prints percentage every PROGRESS_PER percent and '.' every PROGRESS_DOT
#define PROGRESS_PER 20
#define PROGRESS_DOT  2

// We don't have double-evaluations, so simple is OK. Caller should handle types if applicable
#define cc_max(a, b) ((a) > (b) ? (a) : (b))
#define cc_min(a, b) ((a) < (b) ? (a) : (b))
#define cc_crop(a, minval, maxval) cc_max(minval, cc_min(maxval, a))

// from is inclusive, to is exclusive.
typedef struct {
    cc_off_t from;
    cc_off_t to;
} range_t;

void usage(void); // short
void help(void);  // full
cc_off_t fsize(const char* fname);
int get_range(cc_off_t in_size, cc_off_t prev_to, const char *str, range_t *out);

#define VERBOSE(...)  { if (opt_verbose) cc_fprintf(stderr, __VA_ARGS__); }
#define ERR_EXIT(...) { cc_fprintf(stderr, "Error: ");   \
                        cc_fprintf(stderr, __VA_ARGS__); \
                        cc_fprintf(stderr, "\n");        \
                        goto exit_L; }

int main (int argc, char **argv)
{
#ifdef CC_HAVE_WIN_UTF8
    // UTF8 argv. g_win_utf8_enabled affects cc_fopen and cc_fprintf
    argv = win_utf8_argv(argc, argv, &g_win_utf8_enabled);
#endif

    int rv = 1;
    int needs_usage_on_err = 1;

    int opt_verbose = 0;
    int opt_overwrite = 0;
    int opt_progress = 0;
    int opt_dummy = 0;

    char *in_name = NULL;
    char *out_name = NULL;
    FILE *in_file = NULL;
    FILE *out_file = NULL;

    int i = 0;

    opterr = 0; // suppress getopt error prints, we're handling them.
    int c;
    while (1) {
        // This is weird. GNU getopt can be made to work in POSIX compliant mode,
        // but in a way which breaks POSIX compliance...
        // We want to use getopt in standard posix mode, where it doesn't
        // permute argv, and returns -1 when it encounters a non-option.
        // GNU getopt doesn't default to posix mode. It uses posix mode either
        // when the env POSIXLY_CORRECT is set, or when optstring starts with +.
        // But + prefix is a GNU extension - proper posix getopt don't recognize
        // it as an indicator, therefore interpreting it as a valid option char.
        // So to cover both variants, we use the '+' to make GNU posix compliant,
        // but also expect it and then and reject it as an unknown option on posix getopt.
        if ((c = getopt (argc, argv, "+hdvfpo:")) != -1) {
            switch (c) {
                case 'h': help();
                          exit(0);

                case 'v': opt_verbose = 1;
                          break;

                case 'f': opt_overwrite = 1;
                          break;

                case 'p': opt_progress = 1;
                          break;

                case 'd': opt_dummy = 1;
                          break;

                case 'o': out_name = optarg;
                          // Will also exit the while loop and start the ranges
                          break;

                case '+': optopt = '+'; // fallthrough - proper POSIX (bsd, OS X, ...)
                case '?': ERR_EXIT("unknown option -%c%s", optopt,
                                   (optopt >= '0' && optopt <= '9') ?
                                      " (missing -o OUT_FILE before the ranges?)" : "");

                default : ERR_EXIT("(Internal) getopt - unexpected code %d", c);
            }

        } else if (optind < argc) { // still more arguments, so it's a value
            if (!in_name) {
                // still no input file, so this is it.
                in_name = argv[optind];
                optind++; // skip the value and continue parsing.

            } else {
                ERR_EXIT("unexpected '%s' (missing -o OUT_FILE before the ranges?)", argv[optind]);
            }

        } else { // no more arguments to parse
            break;
        }

        if (out_name)  // Once we got the output file, the rest should be ranges
            break;
    }
    // from here onwards, optind should point to the first range in argv

    VERBOSE("- Verbose mode enabled.\n");

    if (opt_overwrite)
        VERBOSE("- Force overwrite output file if exists.\n");

    if (opt_progress)
        VERBOSE("- Progress display enabled.\n");

    if (opt_dummy)
        VERBOSE("- Dummy mode enabled.\n");

    if (!in_name)
        ERR_EXIT("missing input file name");

    if (!out_name)
        ERR_EXIT("missing output file name");

    if (optind == argc)
        ERR_EXIT("no ranges defined, must have at least one range");

    // Input file - verify, open and read size
    cc_off_t in_size = fsize(in_name);
    in_file = cc_fopen(in_name, "rb");
    if (in_size < 0 || !in_file)
        ERR_EXIT("input file '%s' cannot be opened", in_name);
    VERBOSE("-   Input file: '%s', size: %lld\n", in_name, (long long)in_size);

    // verify ranges and calculate expected output size
    cc_off_t expected_output_size = 0;
    cc_off_t prev_to = 0;
    for (i = optind; i < argc; i++) {
        range_t range;
        if (!get_range(in_size, prev_to, argv[i], &range))
            ERR_EXIT("invalid range '%s'", argv[i]);
        expected_output_size += range.to - range.from;
        prev_to = range.to;
        VERBOSE("-   Range #%d: '%s' -> [%lld, %lld) -> %lld bytes\n",
                i - optind + 1,
                argv[i],
                (long long)range.from,
                (long long)range.to,
                (long long)(range.to - range.from)
                );
    }

    if (opt_dummy) {
        VERBOSE("- Done - dummy mode - skipped copying %lld bytes to '%s'%s.\n",
                (long long)expected_output_size, out_name,
                strcmp(out_name, "-") ? "" : " (stdout)");
        rv = 0;
        goto exit_L;
    }

    // open/setup output
    if (!strcmp(out_name, "-")) {
        out_file = stdout;

#ifdef _WIN32
        // change stdout to binary mode, or else it messes with EOL chars
        if (_setmode(_fileno(stdout), O_BINARY) == -1)
            ERR_EXIT("cannot set stdout to binary mode");
#endif

    } else {
        FILE *tmp = cc_fopen(out_name, "r");
        if (tmp) {
            fclose(tmp);
            if (!opt_overwrite)
                ERR_EXIT("output file '%s' exists, use -f to force overwrite", out_name);
        }

        out_file = cc_fopen(out_name, "wb");
        if (!out_file)
            ERR_EXIT("output file '%s' cannot be created", out_name);
    }


    // args are valid, input file is valid, output file created. Start copy
    needs_usage_on_err = 0;
    VERBOSE("- About to copy overall %lld bytes to '%s'%s ...\n",
            (long long)expected_output_size, out_name,
            strcmp(out_name, "-") ? "" : " (stdout)");

    char buf[RW_BUFFSIZE];
    cc_off_t total_processed = 0;
    prev_to = 0;

    for (i = optind; (i < argc) && expected_output_size; i++) {
        range_t range;
        if (!get_range(in_size, prev_to, argv[i], &range))
            ERR_EXIT("(Internal): range became invalid?! '%s'", argv[i]);
        prev_to = range.to;

        if (cc_fseek(in_file, range.from, SEEK_SET))
            ERR_EXIT("cannot seek input file to offset %lld", (long long)range.from);

        cc_off_t toread = range.to - range.from;
        while (toread) {
            size_t single_read = (size_t)(cc_min(toread, (cc_off_t)RW_BUFFSIZE));
            size_t got = fread(buf, 1, single_read, in_file);
            if (ferror(in_file) || got != single_read)
                ERR_EXIT("cannot read from input file");

            if (got != fwrite(buf, 1, got, out_file))
                ERR_EXIT("cannot write to output file");

            toread -= got;
            total_processed += got;

            if (opt_progress) {
                int percent = (int)((double)total_processed / expected_output_size * 100);
                int prev_percent = (int)((double)(total_processed - got) / expected_output_size * 100);

                if (percent / PROGRESS_PER != prev_percent / PROGRESS_PER)
                    cc_fprintf(stderr, " %d%% ", percent);
                else if (percent / PROGRESS_DOT != prev_percent / PROGRESS_DOT)
                    cc_fprintf(stderr, ".");
            }
        }
    }

    if (opt_progress) {
        if (!expected_output_size)
            cc_fprintf(stderr, " %d%% ", 100);
        cc_fprintf(stderr, "\n");
    }

    // success
    VERBOSE("- Done.\n");
    rv = 0;

exit_L:
    if (in_file)
        fclose(in_file);
    if (out_file && out_file != stdout)
        fclose(out_file);

    if (rv && needs_usage_on_err) {
        cc_fprintf(stderr, "\n");
        usage();
    }

#ifdef CC_HAVE_WIN_UTF8
    if (g_win_utf8_enabled)
        free_argvutf8(argc, argv);
#endif

    return rv;
}


///////////////  Utilities, mostly for parsing the ranges safely ///////////////


// out will hold a + b. Return 0 if overflowed, 1 otherwise.
int add_safe(cc_off_t a, cc_off_t b, cc_off_t *out)
{
    int err = 0;
    if (((b > 0) && (a > (OFF_T_MAX - b))) ||
        ((b < 0) && (a < (OFF_T_MIN - b))))
    {
        err = 1;
    }

    *out = a + b;
    return !err;
}

// out will hold a * b. Return 0 if overflowed, 1 otherwise.
int mult_safe(cc_off_t a, cc_off_t b, cc_off_t *out)
{
    int err = 0;
    if (a > 0) {
        if (b > 0) {  /* a and b are positive */
            if (a > (OFF_T_MAX / b)) {
                err = 1;
            }
        } else { /* a positive, b not positive */
            if (b < (OFF_T_MIN / a)) {
                err = 1;
            }
        }
    } else {
        if (b > 0) { /* a is not, b is positive */
            if (a < (OFF_T_MIN / b)) {
                err = 1;
            }
        } else { /* a and b are not positive */
            if ( (a != 0) && (b < (OFF_T_MAX / a))) {
                err = 1;
            }
        }
    }

    *out = a * b;
    return !err;
}

// returns 1 if can safely apply a multiplier suffix. out will hold the new value
int apply_suffix(cc_off_t val, char suffix, cc_off_t *out) {
     cc_off_t mult;
     switch (suffix) {
        // we're assuming cc_off_t can hold 1M
        case 'k': mult = 1000;               break;
        case 'm': mult = 1000 * 1000;        break;
        case 'K': mult = 1024;               break;
        case 'M': mult = 1024 * 1024;        break;
        default : return 0; // unknown suffix
    }

    return mult_safe(val, mult, out);
}

// Returns 0 if cannot parse as a series of decimal digits, possibly followed
// by k/m or K/M to denote a multiplier based on 1000 or 1024 respectively.
// If allow_neg, may be preceded by `-`
int atooff(const char *str, int length, int allow_neg, cc_off_t *outval)
{
    if (!str || length <= 0)
        return 0;

    int is_neg = 0;
    if (*str == '-') {
        if (!allow_neg)
            return 0;

        is_neg = 1;
        str++;
        length--;

        if (length <= 0)
            return 0;
    }

    *outval = 0;
    int first = 1;
    for (; length; length--, str++, first = 0) {
        int digit = *str - '0';

        if (digit < 0 || digit > 9) {
            // abort the digits sequence, and try to apply as a suffix.
            // Only valid if it's the last and not the first and can be applied
            return length == 1 && !first && apply_suffix(*outval, *str, outval);
        }
        first = 0;

        if (is_neg)
            digit = -digit;

        if (!mult_safe(*outval, 10, outval) || !add_safe(*outval, digit, outval))
            return 0;
    }

    return 1;
}

// interpret and reads a range string into out->from and out->to by the syntax:
// [START|+SKIP]:[END|+LENGTH] - START/END/SKIP may be negative, LENGTH may not.
// See help() for behaviour definition.
// Returns 1 on success or 0 on error (at which case out is undefined).
// in_size is the input file size (for cropping or negative START/END)
// prev_to is the previous TO value (for SKIP)
int get_range(cc_off_t in_size, cc_off_t prev_to, const char *str, range_t *out)
{
    if (!out || !str)
        return 0;

    char *sep = strstr(str, ":");
    if (!sep)
        return 0;

    out->from = 0;
    if (sep != str) {
        // FROM exists
        int isfromback = 0;
        int isfromskip = 0;
        if (*str == '-') {
            isfromback = 1; // let atooff read as negative, so no str++
        } else if (*str == '+') {
            isfromskip = 1;
            str++;
        }

        cc_off_t val = 0;
        if (!atooff(str, sep - str, isfromskip || isfromback, &val))
            return 0;

        if (isfromback) {
            if (!add_safe(in_size, val, &out->from))
                return 0; // unreachable since is will never overflow

        } else if (isfromskip) {
            if (!add_safe(prev_to, val, &out->from))
                out->from = OFF_T_MAX; // overflow, use the safe limit.

        } else {
            out->from = val;
        }

        out->from = cc_crop(out->from, 0, in_size);
    }

    sep++; // point to TO
    out->to = in_size;
    if (strlen(sep)) {
        // TO exists
        int istoback = 0;
        int istolen = 0;
        if (*sep == '-') {
            istoback = 1;
        } else if (*sep == '+') {
            istolen = 1;
            sep++;
        }

        cc_off_t val = 0;
        if (!atooff(sep, strlen(sep), istoback, &val))
            return 0;

        if (istoback) {
            if (!add_safe(in_size, val, &out->to))
                return 0; // unreachable since is will never overflow

        } else if (istolen) {
            if (!add_safe(out->from, val, &out->to))
                out->to = OFF_T_MAX; // overflow, use the safe limit

        } else {
            out->to = val;
        }

        out->to = cc_crop(out->to, out->from, in_size);
    }

    return 1;
}

// returns file size or -1 on any error
cc_off_t fsize(const char* fname)
{
    cc_off_t rv = -1;
    FILE *f = cc_fopen(fname, "rb");
    if (!f)
        goto exit_L;
    if (cc_fseek(f, 0, SEEK_END))
        goto exit_L;

    cc_off_t len = cc_ftell(f);
    if (len < 0)
        goto exit_L;

    rv = len;

exit_L:
    if (f)
        fclose(f);
    return rv;
}

void usage()
{
    cc_fprintf(stderr, "\
Usage:   cchunks [-hfvpd] IN_FILE -o OUT_FILE RANGE [RANGE_2 [...]]\n\
Example: Copy 2KiB from offset 5KiB: cchunks infile -o outfile 5K:+2K (or 5K:7K)\n\
Help:    cchunks -h\n\
");
}

void help()
{
    printf("\
Usage: cchunks [-hfvpd] IN_FILE -o OUT_FILE RANGE [RANGE_2 [...]]\n\
Copy chunks from an input file, with flexible ranges description.\n\
Version %s\n\
Values supported: %d bit (%lld - %lld).\n\
\n\
Example: Copy 2KiB from offset 5KiB: cchunks infile -o outfile 5K:+2K (or 5K:7K)\n\
\n\
If OUT_FILE is '-' (without quotes), the output will go to stdout.\n\
Options:\n\
  -h   Display this help and exit.\n\
  -f   Force overwrite OUT_FILE if exists.\n\
  -v   Be verbose (to stderr).\n\
  -p   Print progress (to stderr).\n\
  -d   Dummy mode: validate and resolve inputs, then exit.\n\
\n\
Ranges:\n\
  Ranges may overlap, but will NOT be combined. Ranges are independently copied.\n\
  The output will include the ranges in the order they appear.\n\
  RANGE is in the form of [FROM]:[TO] (without spaces), where:\n\
    FROM is START or +SKIP\n\
    TO   is END   or +LENGTH\n\
  IN_SIZE - the file size of IN_FILE.\n\
  START/END: offset at IN_FILE. If negative, then relative to IN_SIZE.\n\
  SKIP: relative to previous range's TO, may be negative (e.g. '0:50 +-5:100').\n\
  LENGTH: relative to FROM, never negative.\n\
  For convenience, values may use a unit k/m (1000 based) or K/M (1024 based).\n\
  Once resolved, FROM and TO are cropped to [0 .. IN_SIZE] on each RANGE.\n\
  If FROM is omitted, 0 is used. If TO is omitted, IN_SIZE is used.\n\
  If (FROM >= TO), the range is ignored (will not reverse data).\n\
\n\
Sample ranges:\n\
  (up to) 200 bytes from offset 50: '50:250' or '50:+200'\n\
  The first 50 bytes of the file: '0:50' or ':50'\n\
  From offset 50 to EOF: '50:' or '50:-0'\n\
  Everything except the last 50 bytes: '0:-50' or ':-50'\n\
  Last 100 bytes of the file: '-100:' or '-100:-0'\n\
  Take first 100 bytes, skip 2, and take another 100: '0:100 +2:+100'\n\
  The whole file: ':' or '0:-0' or '0:200 +0:' and many others.\n\
  Move the first 100 bytes to the end: '100: :100'\n\
", CCVERSION, (int)sizeof(cc_off_t) * 8, (long long)OFF_T_MIN, (long long)OFF_T_MAX);
}
