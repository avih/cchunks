`getopt.h` and `getopt.c` are verbatim copies of GNU getopt, taken from:
- https://gnunet.org/svn/flightrecorder/src/flightrecorderd/getopt.h
- https://gnunet.org/svn/flightrecorder/src/flightrecorderd/getopt.c

The only modification is line 231 from `# if HAVE_STRING_H` to
`# if HAVE_STRING_H || defined(_WIN32)` in order to include `<string.h>` on
Windows.
