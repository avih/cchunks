# cchunks
Copy chunks from an input file, with flexible ranges description

Build:  `$CC cchunks.c`

On Windows, link with `shell32.lib`, e.g. `cl cchunks.c shell32.lib`

Windows binaries are available at the [Releases](https://github.com/avih/cchunks/releases/) page.

Tested: `gcc` (win/osx/linux), `clang` (osx/linux), `cl` (MSVC), `tcc` (win - no unicode).

```
Usage: cchunks [-hfvpd] IN_FILE -o OUT_FILE RANGE [RANGE_2 [...]]
Copy chunks from an input file, with flexible ranges description.
Version 0.4

Example: Copy 2KiB from offset 5KiB: cchunks infile -o outfile 5K:+2K (or 5K:7K)

If OUT_FILE is '-' (without quotes), the output will go to stdout.
Options:
  -h   Display this help and exit.
  -f   Force overwrite OUT_FILE if exists.
  -v   Be verbose (to stderr).
  -p   Print progress (to stderr).
  -d   Dummy mode: validate and resolve inputs, then exit.

Ranges:
  Ranges may overlap, but will NOT be combined. Ranges are independently copied.
  The output will include the ranges in the order they appear.
  RANGE is in the form of [FROM]:[TO] (without spaces), where:
    FROM is START or +SKIP
    TO   is END   or +LENGTH
  IN_SIZE - the file size of IN_FILE.
  START/END: offset at IN_FILE. If negative, then relative to IN_SIZE.
  SKIP: relative to previous range's TO, may be negative (e.g. '0:50 +-5:100').
  LENGTH: relative to FROM, never negative.
  For convenience, values may use a unit k/m (1000 based) or K/M (1024 based).
  Once resolved, FROM and TO are cropped to [0 .. IN_SIZE] on each RANGE.
  If FROM is omitted, 0 is used. If TO is omitted, IN_SIZE is used.
  If (FROM >= TO), the range is ignored (will not reverse data).

Sample ranges:
  (up to) 200 bytes from offset 50: '50:250' or '50:+200'
  The first 50 bytes of the file: '0:50' or ':50'
  From offset 50 to EOF: '50:' or '50:-0'
  Everything except the last 50 bytes: '0:-50' or ':-50'
  Last 100 bytes of the file: '-100:' or '-100:-0'
  Take first 100 bytes, skip 2, and take another 100: '0:100 +2:+100'
  The whole file: ':' or '0:-0' or '0:200 +0:' and many others.
  Move the first 100 bytes to the end: '100: :100'
```
