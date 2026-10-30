/* The Makefile builds expat and its xmlwf with this header in front of each
   file; xmlwf.c alone opens files with fopen(). With "-d DIRECTORY" xmlwf
   writes what it parses to DIRECTORY/NAME, for each file NAME, and with
   "-d -" this sends it to stdout instead, where the runner compares it.
   Several files then follow each other there, so stdout is flushed where
   xmlwf would close that file, and keeps its own buffer. */
#include <stdio.h>
#include <string.h>

static inline int xmlwf_to_stdout(const char *name)
{
    return strncmp(name, "-/", 2) == 0;
}

static inline FILE *xmlwf_fopen(const char *name, const char *mode)
{
    return xmlwf_to_stdout(name) ? stdout : fopen(name, mode);
}

static inline int xmlwf_fclose(FILE *fp)
{
    return fp == stdout ? fflush(fp) : fclose(fp);
}

static inline int xmlwf_setvbuf(FILE *fp, char *buf, int mode, size_t size)
{
    return fp == stdout ? 0 : setvbuf(fp, buf, mode, size);
}

#define fopen xmlwf_fopen
#define fclose xmlwf_fclose
#define setvbuf xmlwf_setvbuf
