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
*  Build: $CC cchunks.c gnu-getopt/getopt.c -o cchunks
*  Tested $CC as: gcc (win/osx/linux), clang (osx/linux), tcc (win), cl (MS compiler)
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
    #define snprintf _snprintf
#endif

#ifdef _WIN32
    // io.h needed for _telli64 in tcc, fcntl.h for O_BINARY in tcc
    // _ftelli64 is hard to link. _telli64 + _fileno work the same, easier to link
    #include <io.h>
    #include <fcntl.h>

    #define cc_off_t __int64
    #define cc_fseek _fseeki64
    #define cc_ftell(fd) _telli64(_fileno(fd))

#else
    #define cc_off_t off_t
    #define cc_fseek fseeko
    #define cc_ftell ftello

#endif

// Using a private copy of gnu getopt
#include "gnu-getopt/getopt.h"

#define CCVERSION "0.3"
#define MAX_DIGITS 10
#define RW_BUFFSIZE (512 * 1024)

// int, in percentage, e.g. 2 will produce up to 50 progress marks.
// May print less if the buffer is big compared to the expected output size
#define PROGRESS_EVERY 2

void usage(FILE* fd)
{
  fprintf(fd, "\
Usage: cchunks [-hfvp] IN_FILE -o OUT_FILE RANGE [RANGE_2 [...]]\n\
Copy chunks from an input file, with flexible ranges description.\n\
Version %s\n\
\n\
Example: Copy 200 bytes from offset 50: cchunks myfile -o outfile 50:+200 \n\
\n\
If OUT_FILE is '-' (without quotes), the output will go to stdout.\n\
Options:\n\
  -f   Force overwrite OUT_FILE if exists.\n\
  -v   Be verbose (to stderr).\n\
  -p   Print progress (to stderr).\n\
  -h   Display this help and exit (also if no arguments).\n\
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
  FROM and TO are cropped to [0 .. IN_SIZE]\n\
  If FROM is omitted, 0 is used. If TO is omitted, IN_SIZE is used.\n\
  If FROM is not smaller than TO, the range is ignored (will not reverse data).\n\
  If the output ends up empty, an empty OUT_FILE will be created.\n\
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
", CCVERSION);
}

// We don't have double-evaluations, so simple is OK.
#define mymax(a, b) ((a) > (b) ? (a) : (b))
#define mymin(a, b) ((a) < (b) ? (a) : (b))
#define mycrop(a, minval, maxval) mymax(minval, mymin(maxval, a))

typedef struct _range_t {
    cc_off_t from;
    cc_off_t to;
} range_t;

// returns file size or -1 on any error
cc_off_t fsize(const char* fname)
{
    cc_off_t rv = -1;
    FILE *f = fopen(fname, "rb");
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

// returns 0 if cannot parse as a series of decimal digits,
// if allow_neg, may be preceded by `-`
int atooff_t(const char *str, int length, int allow_neg, cc_off_t *outval)
{
    if (!str || length <= 0)
        return 0;

    int is_neg = 0;
    if (allow_neg && *str == '-') {
        is_neg = 1;
        str++;
        length--;
    }

    if (length <= 0 || length > MAX_DIGITS)
        return 0;

    *outval = 0;
    for (; length; length--, str++) {
        if (*str < '0' || *str > '9')
            return 0;

        *outval = *outval * 10 + (*str - '0');
    }

    if (is_neg)
        *outval = -*outval;

    return 1;
}

// interpret and reads a range string into out->from and out->to by the syntax:
// [start|+skip]:[end|+length]
// See usage() for behaviour definition.
// Returns 1 on success or 0 on error (at which case out is undefined).
// in_size is the input file size (for cropping or negative START/END)
// prev_to is the previous TO value (for SKIP)
int get_range(cc_off_t in_size, cc_off_t prev_to, const char *range, range_t *out)
{
    if (!out || !range || !*range)
        return 0;

    char *sep = strstr(range, ":");
    if (!sep)
        return 0;

    out->from = 0;
    if (sep != range) {
        // FROM exists
        int isfromback = 0;
        int isfromprev = 0;
        if (*range == '-') {
            isfromback = 1;
            range++;
        } else if (*range == '+') {
            isfromprev = 1;
            range++;
        }

        // SKIP allows negative values, but should still use +, e.g. +-10
        if (!atooff_t(range, sep - range, isfromprev, &out->from))
            return 0;

        if (isfromback)
            out->from = in_size - out->from;
        else if (isfromprev)
            out->from = prev_to + out->from;

        out->from = mycrop(out->from, 0, in_size);
    }

    sep++; // point to TO
    out->to = in_size;
    if (strlen(sep)) {
        // TO exists
        int istoback = 0;
        int istolen = 0;
        if (*sep == '-') {
            istoback = 1;
            sep++;
        } else if (*sep == '+') {
            istolen = 1;
            sep++;
        }

        if (!atooff_t(sep, strlen(sep), 0, &out->to))
            return 0;

        if (istoback)
            out->to = in_size - out->to;
        else if (istolen)
            out->to = out->from + out->to;

        out->to = mycrop(out->to, out->from, in_size);
    }

    return 1;
}

#define MSG_LEN 255
// Both VERBOSE and ERR_EXIT have printf-like args, but VERBOSE doesn't add
// a trailing \n, while ERR_EXIT prefixes "Error: ", adds \n and exits cchunks.
#define VERBOSE(...) { if (opt_verbose) fprintf(stderr, __VA_ARGS__); }
#define ERR_EXIT(...) { snprintf(errmsg, MSG_LEN, __VA_ARGS__); goto exit_L; }

int main (int argc, char* argv[])
{
    if (argc == 1) {
        usage(stdout);
        exit(0);
    }

    int rv = 1;
    char errmsg[MSG_LEN + 1] = {0};
    int needs_usage_on_err = 1;

    int opt_verbose = 0;
    int opt_overwrite = 0;
    int opt_progress = 0;
    char *ifname = NULL;
    char *ofname = NULL;

    FILE *in_file = NULL;
    FILE *out_file = NULL;
    int i = 0;

    opterr = 0; // we're printing our own error messages
    int c;
    // options starts with '-' to allow unrecognised options (for IN_FILE)
    while ((c = getopt (argc, argv, "-hvfpo:")) != -1) {
        switch (c) {
            case 'h': usage(stdout);
                      exit(0);

            case 'v': opt_verbose = 1;
                      break;

            case 'f': opt_overwrite = 1;
                      break;

            case 'p': opt_progress = 1;
                      break;

            case 'o': ofname = optarg;
                      // Will also exit the while loop and continue to the ranges
                      break;

            case '?': ERR_EXIT("invalid option -%c", optopt);

            default : // We expect a single non-option (IN_FILE) before -o
                      if (ifname)
                          ERR_EXIT("unexpected argument '%s' (missing -o OUT_FILE before the ranges?)", optarg);
                      ifname = optarg;
                      break;
        }

        if (ofname)  // Once we got the output file, the rest should be ranges
            break;
    }
    // from here onwards, optind points to the first range in argv

    VERBOSE("- Setting verbose mode.\n");

    if (opt_overwrite)
        VERBOSE("- Overwrite output file if exists.\n");

    if (opt_progress)
        VERBOSE("- Enabling progress display.\n");

    if (!ifname)
        ERR_EXIT("missing input file name.");

    if (!ofname)
        ERR_EXIT("missing output file name.");

    if (optind == argc)
        ERR_EXIT("no ranges defined, must have at least one range.");

    // Input file - verify, open and read size
    cc_off_t in_size = fsize(ifname);
    in_file = fopen(ifname, "rb");
    if (in_size < 0 || !in_file)
        ERR_EXIT(" input file '%s' cannot be opened", ifname);
    VERBOSE("- Input file: '%s', size: %lld\n", ifname, (long long)in_size);

    // verify ranges and calculate expected output size
    cc_off_t expected_output_size = 0;
    cc_off_t prev_to = 0;
    for (i = optind; i < argc; i++) {
        range_t range;
        if (!get_range(in_size, prev_to, argv[i], &range))
            ERR_EXIT("invalid range '%s'", argv[i]);
        expected_output_size += range.to - range.from;
        prev_to = range.to;
        VERBOSE("- Range #%d: '%s' -> [%lld, %lld) -> %lld bytes\n",
                i - optind + 1,
                argv[i],
                (long long)range.from,
                (long long)range.to,
                (long long)(range.to - range.from)
                );
    }

    // open/setup output
    if (!strcmp(ofname, "-")) {
        out_file = stdout;
        VERBOSE("- Output goes to stdout.\n");

#ifdef _WIN32
        // change stdout to binary mode on windows, or else it messes with LF/CRLF
        VERBOSE("- Setting stdout to binary mode.\n");
        if (_setmode(_fileno(stdout), O_BINARY) == -1)
            ERR_EXIT("Cannot set stdout to binary mode.");
#endif

    } else {
        if (fsize(ofname) >= 0 && !opt_overwrite)
            ERR_EXIT("output file '%s' exists and -f was not specified", ofname);

        out_file = fopen(ofname, "wb");
        if (!out_file)
            ERR_EXIT("output file '%s' cannot be created", ofname);
        VERBOSE("- Output file '%s' created.\n", ofname);
    }


    // args are valid, input file is valid, output file created. Start copy
    needs_usage_on_err = 0;
    VERBOSE("- Copying chunks ...\n");

    char buf[RW_BUFFSIZE];
    cc_off_t total_processed = 0;
    prev_to = 0;

    for (i = optind; i < argc; i++) {
        range_t range;
        if (!get_range(in_size, prev_to, argv[i], &range))
            ERR_EXIT("(Internal): range became invalid?! '%s'", argv[i]);
        prev_to = range.to;

        if (cc_fseek(in_file, range.from, SEEK_SET))
            ERR_EXIT("cannot seek input file to offset %lld", (long long)range.from);

        cc_off_t toread = range.to - range.from;
        while (toread) {
            if (toread < 0)
                ERR_EXIT("(Internal): unexpected negative size to read");

            cc_off_t got = fread(buf, 1, mymin(toread, RW_BUFFSIZE), in_file);
            if (ferror(in_file) || got != mymin(toread, RW_BUFFSIZE))
                ERR_EXIT("cannot read from input file");

            if (got != fwrite(buf, 1, got, out_file))
                ERR_EXIT("cannot write to output file");

            toread -= got;
            total_processed += got;

            if (opt_progress) {
                int percent = (double)total_processed / expected_output_size * 100;
                int prev_percent = (double)(total_processed - got) / expected_output_size * 100;

                if (percent / PROGRESS_EVERY != prev_percent / PROGRESS_EVERY)
                    fprintf(stderr, ".");
                if (percent / 10 != prev_percent / 10)
                    fprintf(stderr, " %d%% ", percent);
            }
        }
    }

    if (opt_progress)
        fprintf(stderr, "\n");
    VERBOSE("- Done. Wrote %lld bytes.\n", (long long)total_processed);

    // success
    rv = 0;

exit_L:
    if (in_file)
        fclose(in_file);
    if (out_file && out_file != stdout)
        fclose(out_file);

    if (strlen(errmsg)) {
        fprintf(stderr, "Error: ");
        fprintf(stderr, "%s\n", errmsg);
    }

    if (rv && needs_usage_on_err) {
        if (strlen(errmsg))
            fprintf(stderr, "\n");
        usage(stderr);
    }

    return rv;
}
