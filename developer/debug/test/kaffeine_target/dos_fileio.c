/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    dos.library file, lock, path and variable functions, called from a
    libnix C program that uses neither stdio nor malloc. The file is
    written and read back with Open(), Write(), Read(), Seek() and Close().
    VFPrintf() output is read back too: the bridge renders it with 68k
    RawDoFmt rules and refuses bad strings and lengths before native DOS.
    MatchFirst() and MatchNext() walk a directory and one below it.
    Check the converted results of ExamineFH(), DateStamp(), StrToLong(),
    FilePart(), PathPart() and FGets(), including guest pointer identity and
    aliasing.
    Historically blocking DOS calls are isolated in dos_lvo.c.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>
#include <string.h>

#include "m68ktest.h"

#define DIR_NAME    "RAM:kaffeine-test"
#define FILE_NAME   DIR_NAME "/data.bin"
#define FILE_RENAME DIR_NAME "/renamed.bin"
#define BLOCK_NAME  DIR_NAME "/blocks.bin"
#define PRINT_NAME  DIR_NAME "/printed.txt"
#define MATCH_A     DIR_NAME "/match-a.txt"
#define MATCH_B     DIR_NAME "/match-b.txt"
#define MATCH_DIR   DIR_NAME "/matchdir"
#define MATCH_INNER MATCH_DIR "/inner.txt"
#define MATCH_BUF   256
#define FILE_SIZE   1000
#define TEXT_NAME   "RAM:kaffeine-fgets.txt"
#define TEXT_LINES  "first line\nsecond\nlast"

static UBYTE out[FILE_SIZE], in[FILE_SIZE];
static struct FileInfoBlock fib __attribute__((aligned(4)));
static char buf[256];
static char tokens[64];
static UBYTE anchor_space[sizeof(struct AnchorPath) + MATCH_BUF] __attribute__((aligned(4)));

/* The bridge copies these fields at the offsets of the NDK layout. */
typedef char ap_info_at_20[offsetof(struct AnchorPath, ap_Info) == 20 ? 1 : -1];
typedef char ap_buf_at_280[offsetof(struct AnchorPath, ap_Buf) == 280 ? 1 : -1];
typedef char an_lock_at_8[offsetof(struct AChain, an_Lock) == 8 ? 1 : -1];
typedef char an_string_at_273[offsetof(struct AChain, an_String) == 273 ? 1 : -1];

static BOOL ends_with(CONST_STRPTR s, CONST_STRPTR tail)
{
    LONG ls = strlen(s), lt = strlen(tail);

    return ls >= lt && strcmp(s + ls - lt, tail) == 0;
}

static BOOL write_file(CONST_STRPTR name, LONG size)
{
    BPTR fh = Open(name, MODE_NEWFILE);
    LONG n;

    if (!fh)
        return FALSE;
    n = Write(fh, out, size);
    return Close(fh) && n == size;
}

/* MatchFirst() and MatchNext() run on a native AnchorPath on the bridge.
   The program sees ap_Info, ap_Buf and the flags, and in ap_Base and
   ap_Current AChain copies whose an_Lock works with CurrentDir() and
   NameFromLock(). IoErr() is read before anything is printed. */
static void test_match(void)
{
    struct AnchorPath *ap = (struct AnchorPath *)anchor_space;
    CONST_STRPTR name;
    BPTR dir, lock, old;
    LONG error, found = 0, entered = 0, left = 0, inner = 0, locks = 0, ioerr;

    check(write_file(MATCH_A, 3) && write_file(MATCH_B, 5), "files for MatchFirst()");
    dir = CreateDir(MATCH_DIR);
    if (dir)
        UnLock(dir);
    check(dir != 0 && write_file(MATCH_INNER, 7), "a directory for APF_DODIR");

    memset(anchor_space, 0, sizeof(anchor_space));
    ap->ap_Strlen = MATCH_BUF;
    for (error = MatchFirst(DIR_NAME "/match-#?.txt", ap); !error; error = MatchNext(ap))
    {
        name = (CONST_STRPTR)ap->ap_Info.fib_FileName;
        if (!strcmp((const char *)name, "match-a.txt"))
        {
            found |= 1;
            check_long("MatchFirst() ap_Info fib_Size of match-a.txt", ap->ap_Info.fib_Size, 3);
            check(ends_with((CONST_STRPTR)ap->ap_Buf, "kaffeine-test/match-a.txt"),
                  "ap_Buf holds the path of match-a.txt");
        }
        else if (!strcmp((const char *)name, "match-b.txt"))
        {
            found |= 2;
            check_long("MatchNext() ap_Info fib_Size of match-b.txt", ap->ap_Info.fib_Size, 5);
            check(ends_with((CONST_STRPTR)ap->ap_Buf, "kaffeine-test/match-b.txt"),
                  "ap_Buf holds the path of match-b.txt");
        }
        else
            found |= 4;
        buf[0] = 0;
        if (ap->ap_Current && NameFromLock(ap->ap_Current->an_Lock, (STRPTR)buf, sizeof(buf)) &&
            ends_with(buf, "kaffeine-test"))
        {
            old = CurrentDir(ap->ap_Current->an_Lock);
            lock = Lock(name, SHARED_LOCK);
            CurrentDir(old);
            if (lock)
            {
                locks++;
                UnLock(lock);
            }
        }
    }
    check_long("MatchNext() ends with ERROR_NO_MORE_ENTRIES", error, ERROR_NO_MORE_ENTRIES);
    check_long("MatchFirst() and MatchNext() found the two files", found, 3);
    check_long("ap_Current->an_Lock names the directory and works with CurrentDir()", locks, 2);
    MatchEnd(ap);
    check(!ap->ap_Base && !ap->ap_Current, "MatchEnd() clears ap_Base and ap_Current");

    memset(anchor_space, 0, sizeof(anchor_space));
    ap->ap_Strlen = MATCH_BUF;
    for (error = MatchFirst(DIR_NAME "/#?", ap); !error; error = MatchNext(ap))
    {
        name = (CONST_STRPTR)ap->ap_Info.fib_FileName;
        if (ap->ap_Info.fib_DirEntryType > 0)
        {
            if (ap->ap_Flags & APF_DIDDIR)
                left++;
            else
            {
                entered++;
                ap->ap_Flags |= APF_DODIR;
            }
        }
        else if (!strcmp((const char *)name, "inner.txt"))
        {
            buf[0] = 0;
            inner = ends_with((CONST_STRPTR)ap->ap_Buf, "kaffeine-test/matchdir/inner.txt") &&
                NameFromLock(ap->ap_Current->an_Lock, (STRPTR)buf, sizeof(buf)) &&
                ends_with(buf, "matchdir");
        }
    }
    check_long("MatchNext() through a directory ends with ERROR_NO_MORE_ENTRIES",
               error, ERROR_NO_MORE_ENTRIES);
    check_long("APF_DODIR entered one directory", entered, 1);
    check_long("APF_DIDDIR reported leaving it", left, 1);
    check(inner, "the file inside has its path in ap_Buf and its directory in an_Lock");
    MatchEnd(ap);

    ap->ap_Strlen = -1;
    error = MatchFirst(DIR_NAME "/#?", ap);
    ioerr = IoErr();
    check_long("MatchFirst() with a negative ap_Strlen", error, ERROR_BAD_NUMBER);
    check_long("IoErr() after the refused MatchFirst()", ioerr, ERROR_BAD_NUMBER);
    ap->ap_Strlen = MATCH_BUF;
    check_long("MatchNext() without an open match", MatchNext(ap), ERROR_BAD_NUMBER);
    error = MatchFirst("RAM:kaffeine-no-such-dir/#?", ap);
    check_long("MatchFirst() in a missing directory", error, ERROR_OBJECT_NOT_FOUND);
    MatchEnd(ap);

    check(DeleteFile(MATCH_INNER) && DeleteFile(MATCH_DIR) && DeleteFile(MATCH_A) &&
          DeleteFile(MATCH_B), "DeleteFile() of the MatchFirst() files");
}

int main(void)
{
    BPTR fh, lock, dir;
    struct FileInfoBlock *dfib = NULL;
    struct DateStamp ds;
    LONG i, n, value, error;
    CONST_STRPTR path;
    STRPTR part;
    UBYTE binary_value[] = { 'A', 0, '\n', 0x7F };
    char small[8];

    for (i = 0; i < FILE_SIZE; i++)
        out[i] = (UBYTE)(i * 7 + 3);

    dir = CreateDir(DIR_NAME);
    check(dir != 0, "CreateDir() returns a lock");
    if (dir)
        UnLock(dir);

    /* Write the file, then read it back */
    fh = Open(FILE_NAME, MODE_NEWFILE);
    if (check(fh != 0, "Open(MODE_NEWFILE)"))
    {
        check_long("Write() of 1000 bytes", Write(fh, out, FILE_SIZE), FILE_SIZE);
        check_long("Seek(OFFSET_CURRENT) after the write", Seek(fh, 0, OFFSET_CURRENT), FILE_SIZE);
        check(Close(fh) != 0, "Close() after writing");
    }

    fh = Open(FILE_NAME, MODE_OLDFILE);
    if (check(fh != 0, "Open(MODE_OLDFILE)"))
    {
        check_long("Read() of 1000 bytes", Read(fh, in, FILE_SIZE), FILE_SIZE);
        check(memcmp(in, out, FILE_SIZE) == 0, "data read back equals data written");
        check_long("Read() at the end of the file", Read(fh, in, 10), 0);
        check_long("Seek(100, OFFSET_BEGINNING) returns the old position",
                   Seek(fh, 100, OFFSET_BEGINNING), FILE_SIZE);
        check_long("Read() of 10 bytes at offset 100", Read(fh, in, 10), 10);
        check(memcmp(in, out + 100, 10) == 0, "data at offset 100");
        check_long("Seek(0, OFFSET_END) returns the old position", Seek(fh, 0, OFFSET_END), 110);
        check_long("Seek(0, OFFSET_CURRENT) at the end", Seek(fh, 0, OFFSET_CURRENT), FILE_SIZE);

        /* The thunk converts the native FIB to the classic 68k layout. */
        memset(&fib, 0, sizeof(fib));
        if (check(ExamineFH(fh, &fib) != 0, "ExamineFH()"))
        {
            check_long("ExamineFH() fib_Size", fib.fib_Size, FILE_SIZE);
            check_str("ExamineFH() fib_FileName", (CONST_STRPTR)fib.fib_FileName, "data.bin");
            check(fib.fib_DirEntryType < 0, "ExamineFH() fib_DirEntryType says file");
        }
        check(Close(fh) != 0, "Close() after reading");
    }

    /* Buffered I/O returns block counts, including zero at EOF. */
    fh = Open(BLOCK_NAME, MODE_NEWFILE);
    if (check(fh != 0, "Open() for FWrite()"))
    {
        check_long("FWrite() of 100 ten-byte blocks", FWrite(fh, out, 10, FILE_SIZE / 10), FILE_SIZE / 10);
        check(Close(fh) != 0, "Close() flushes FWrite()");
    }
    memset(in, 0, sizeof(in));
    fh = Open(BLOCK_NAME, MODE_OLDFILE);
    if (check(fh != 0, "Open() for FRead()"))
    {
        check_long("FRead() of 100 ten-byte blocks", FRead(fh, in, 10, FILE_SIZE / 10), FILE_SIZE / 10);
        check(memcmp(in, out, FILE_SIZE) == 0, "FWrite()/FRead() binary round trip");
        check_long("FRead() at EOF", FRead(fh, in, 10, 1), 0);
        buf[0] = 0;
        check(NameFromFH(fh, buf, sizeof(buf)) != 0, "NameFromFH()");
        check(ends_with(buf, "kaffeine-test/blocks.bin"), "NameFromFH() ends with the file's path");
        check(Close(fh) != 0, "Close() after FRead()");
    }
    check(DeleteFile(BLOCK_NAME) != 0, "DeleteFile() of buffered I/O file");

    /* A word for %d and %c unless 'l' asks for a long; a zero byte is
       neither written nor counted. A string outside guest memory, a
       negative length and a NULL name fail before native DOS, with
       ERROR_BAD_NUMBER; IoErr() is read before anything is printed. */
    fh = Open(PRINT_NAME, MODE_NEWFILE);
    if (check(fh != 0, "Open() for VFPrintf()"))
    {
        ULONG text = (ULONG)"text";
        UWORD stream[6] = { 0xFFFF, 123456 >> 16, 123456 & 0xFFFF, 0, 0, 'Z' };

        stream[3] = text >> 16;
        stream[4] = text & 0xFFFF;
        check_long("VFPrintf() with word and long arguments",
                   VFPrintf(fh, "%d|%ld|%s|%c\n", stream), 17);
        stream[0] = 0;
        check_long("VFPrintf() leaves out a zero character", VFPrintf(fh, "a%cb", stream), 2);
        stream[0] = 0xFFFF;
        stream[1] = 0xFFF0;
        n = VFPrintf(fh, "lost %s", stream);
        error = IoErr();
        check_long("VFPrintf() with a string outside guest memory", n, -1);
        check_long("IoErr() after the refused VFPrintf()", error, ERROR_BAD_NUMBER);
        check_long("Write() of zero bytes", Write(fh, NULL, 0), 0);
        n = Write(fh, out, -1);
        error = IoErr();
        check_long("Write() with a negative length", n, -1);
        check_long("IoErr() after the negative length", error, ERROR_BAD_NUMBER);
        check(Close(fh) != 0, "Close() after VFPrintf()");
    }
    memset(buf, 0, sizeof(buf));
    fh = Open(PRINT_NAME, MODE_OLDFILE);
    if (check(fh != 0, "Open() of the VFPrintf() output"))
    {
        check_long("Read() of the VFPrintf() output", Read(fh, buf, sizeof(buf) - 1), 19);
        check_str("VFPrintf() output read back", buf, "-1|123456|text|Z\nab");
        check_long("Read() of zero bytes", Read(fh, NULL, 0), 0);
        check(Close(fh) != 0, "Close() after reading the VFPrintf() output");
    }
    check(DeleteFile(PRINT_NAME) != 0, "DeleteFile() of the VFPrintf() output");
    fh = Open(NULL, MODE_OLDFILE);
    error = IoErr();
    check(fh == 0, "Open(NULL) fails");
    if (fh)
        Close(fh);
    else
        check_long("IoErr() after Open(NULL)", error, ERROR_BAD_NUMBER);

    check(SetComment(FILE_NAME, "guest DOS comment") != 0, "SetComment()");
    check(SetProtection(FILE_NAME, FIBF_SCRIPT) != 0, "SetProtection(FIBF_SCRIPT)");

    /* Examine() has a manual thunk that converts the FileInfoBlock */
    lock = Lock(FILE_NAME, SHARED_LOCK);
    if (check(lock != 0, "Lock(SHARED_LOCK)"))
    {
        memset(&fib, 0, sizeof(fib));
        if (check(Examine(lock, &fib) != 0, "Examine() with a FileInfoBlock in BSS"))
        {
            check_long("Examine() fib_Size", fib.fib_Size, FILE_SIZE);
            check_str("Examine() fib_FileName", (CONST_STRPTR)fib.fib_FileName, "data.bin");
            check(fib.fib_DirEntryType < 0, "Examine() fib_DirEntryType says file");
            check_str("SetComment() read back through Examine()", (CONST_STRPTR)fib.fib_Comment,
                      "guest DOS comment");
            check((fib.fib_Protection & FIBF_SCRIPT) != 0, "SetProtection() read back through Examine()");
        }

        dfib = AllocDosObject(DOS_FIB, NULL);
        if (check(dfib != NULL, "AllocDosObject(DOS_FIB)"))
        {
            if (check(Examine(lock, dfib) != 0, "Examine() with a FileInfoBlock from AllocDosObject()"))
            {
                check_long("Examine() fib_Size", dfib->fib_Size, FILE_SIZE);
                check_str("Examine() fib_FileName", (CONST_STRPTR)dfib->fib_FileName, "data.bin");
            }
        }

        buf[0] = 0;
        check(NameFromLock(lock, (STRPTR)buf, sizeof(buf)) != 0, "NameFromLock()");
        check(ends_with(buf, "kaffeine-test/data.bin"), "NameFromLock() ends with kaffeine-test/data.bin");
        Printf("     NameFromLock(): \"%s\"\n", (LONG)buf);
        UnLock(lock);
    }
    check(SetProtection(FILE_NAME, 0) != 0, "SetProtection() clears the test bits");

    /* Rename, then check the old name is gone. IoErr() is read before
       anything is printed, because printing changes it. */
    check(Rename(FILE_NAME, FILE_RENAME) != 0, "Rename()");
    lock = Lock(FILE_NAME, SHARED_LOCK);
    error = IoErr();
    check(lock == 0, "Lock() of the old name fails after Rename()");
    if (lock)
        UnLock(lock);
    else
        check_long("IoErr() after the failed Lock()", error, ERROR_OBJECT_NOT_FOUND);

    n = Fault(ERROR_OBJECT_NOT_FOUND, NULL, (STRPTR)buf, sizeof(buf));
    check(n > 0, "Fault() writes a message");
    check(buf[0] != 0, "Fault() produces nonempty text");
    /* rom/dos/fault.c returns unused capacity, despite its RESULT comment. */
    check_long("Fault() native return is remaining capacity", n, sizeof(buf) - strlen(buf));
    Printf("     Fault(ERROR_OBJECT_NOT_FOUND): \"%s\"\n", (LONG)buf);
    n = Fault(ERROR_OBJECT_NOT_FOUND, "probe", buf, sizeof(buf));
    check(strncmp(buf, "probe: ", 7) == 0, "Fault() prepends its header");
    check_long("Fault() with a header returns remaining capacity", n, sizeof(buf) - strlen(buf));
    memset(small, 0x5A, sizeof(small));
    n = Fault(ERROR_OBJECT_NOT_FOUND, NULL, small, 4);
    check_long("Fault() truncated return", n, 1);
    check(small[3] == 0 && small[4] == 0x5A, "Fault() terminates a truncated buffer");
    n = PrintFault(0, NULL);
    error = IoErr();
    check(n != 0, "PrintFault(0, NULL)");
    check_long("PrintFault() leaves the supplied code in IoErr()", error, 0);

    check(DeleteFile(FILE_RENAME) != 0, "DeleteFile() of the file");
    fh = Open(FILE_RENAME, MODE_OLDFILE);
    error = IoErr();
    check(fh == 0, "Open(MODE_OLDFILE) fails after DeleteFile()");
    if (fh)
        Close(fh);
    else
        check_long("IoErr() after the failed Open()", error, ERROR_OBJECT_NOT_FOUND);
    test_match();
    check(DeleteFile(DIR_NAME) != 0, "DeleteFile() of the directory");

    /* Local shell variables: strings only */
    check(SetVar("kaffeine_test", "value 42", -1, GVF_LOCAL_ONLY) != 0, "SetVar()");
    buf[0] = 0;
    check_long("GetVar() length", GetVar("kaffeine_test", (STRPTR)buf, sizeof(buf), GVF_LOCAL_ONLY), 8);
    check_str("GetVar() value", (CONST_STRPTR)buf, "value 42");
    memset(small, 0x5A, sizeof(small));
    n = GetVar("kaffeine_test", small, 4, GVF_LOCAL_ONLY);
    error = IoErr();
    check_long("GetVar() truncated length", n, 3);
    check_long("GetVar() full length in IoErr()", error, 8);
    check_str("GetVar() truncated value", small, "val");
    check(small[4] == 0x5A, "GetVar() leaves the byte after a short buffer");
    check(SetVar("kaffeine_test", binary_value, sizeof(binary_value), GVF_LOCAL_ONLY | GVF_BINARY_VAR) != 0,
          "SetVar() of explicit binary bytes");
    n = GetVar("kaffeine_test", small, sizeof(binary_value),
               GVF_LOCAL_ONLY | GVF_BINARY_VAR | GVF_DONT_NULL_TERM);
    error = IoErr();
    check_long("GetVar() binary length", n, sizeof(binary_value));
    check_long("GetVar() binary full length in IoErr()", error, sizeof(binary_value));
    check(memcmp(small, binary_value, sizeof(binary_value)) == 0,
          "GetVar() binary mode preserves NUL and newline");
    check(DeleteVar("kaffeine_test", GVF_LOCAL_ONLY) != 0, "DeleteVar()");

    strcpy(buf, "RAM:dir");
    check(AddPart((STRPTR)buf, "file", sizeof(buf)) != 0, "AddPart()");
    check_str("AddPart() result", (CONST_STRPTR)buf, "RAM:dir/file");
    strcpy(small, "RAM:dir");
    n = AddPart(small, "file", sizeof(small));
    error = IoErr();
    check(n == 0, "AddPart() fails when the result does not fit");
    check_long("AddPart() no-fit IoErr()", error, ERROR_LINE_TOO_LONG);
    check_str("AddPart() leaves a short buffer intact", small, "RAM:dir");

    check_long("ParsePattern() detects wildcards", ParsePattern("file#?.txt", tokens, sizeof(tokens)), 1);
    check(MatchPattern(tokens, "file12.txt") != 0, "MatchPattern() matches native tokens");
    check(MatchPattern(tokens, "file12.bin") == 0, "MatchPattern() rejects another extension");
    check(MatchPattern(tokens, "FILE12.TXT") == 0, "MatchPattern() is case sensitive");
    check_long("ParsePattern() literal result", ParsePattern("plain", tokens, sizeof(tokens)), 0);
    check(MatchPattern(tokens, "plain") != 0, "MatchPattern() accepts a literal pattern");
    check_long("ParsePatternNoCase() detects wildcards",
               ParsePatternNoCase("FiLe#?.TxT", tokens, sizeof(tokens)), 1);
    check(MatchPatternNoCase(tokens, "FILE12.TXT") != 0, "MatchPatternNoCase() accepts uppercase");
    check(MatchPatternNoCase(tokens, "file12.txt") != 0, "MatchPatternNoCase() accepts lowercase");
    check(MatchPatternNoCase(tokens, "file12.bin") == 0, "MatchPatternNoCase() rejects another extension");

    /* SetIoErr() returns the previous value */
    SetIoErr(1234);
    check_long("SetIoErr() returns the previous value", SetIoErr(0), 1234);

    /* Converted LONG output, including a failed parse that writes zero. */
    value = 0;
    check_long("StrToLong() characters used", StrToLong("12345", &value), 5);
    check_long("StrToLong() value", value, 12345);
    check_long("StrToLong() whitespace and sign", StrToLong(" \t-42tail", &value), 5);
    check_long("StrToLong() negative value", value, -42);
    value = 12345;
    check_long("StrToLong() without digits", StrToLong("tail", &value), -1);
    check_long("StrToLong() clears output without digits", value, 0);

    path = "RAM:a/b/c.txt";
    check_long("FilePart() returns a pointer into the path",
               (LONG)FilePart(path), (LONG)(path + 8));
    path = "";
    check_long("FilePart() of an empty path", (LONG)FilePart(path), (LONG)path);
    path = "RAM:dir/";
    check_long("FilePart() after a trailing separator",
               (LONG)FilePart(path), (LONG)(path + strlen(path)));
    strcpy(buf, "RAM:a/file");
    part = FilePart((STRPTR)buf);
    if (check(part == (STRPTR)buf + 6, "FilePart() preserves the original buffer"))
    {
        part[0] = 'F';
        check_str("FilePart() result aliases the original path", (CONST_STRPTR)buf, "RAM:a/File");
    }

    /* PathPart() points at the last '/', or after the last ':' */
    path = "RAM:a/b/c.txt";
    check_long("PathPart() returns the last separator", (LONG)PathPart(path), (LONG)(path + 7));
    path = "RAM:file";
    check_long("PathPart() after a volume name", (LONG)PathPart(path), (LONG)(path + 4));
    path = "";
    check_long("PathPart() of an empty path", (LONG)PathPart(path), (LONG)path);
    path = "RAM:";
    check_long("PathPart() of a volume name returns its terminator",
               (LONG)PathPart(path), (LONG)(path + 4));
    strcpy(buf, "RAM:a/file");
    part = PathPart((STRPTR)buf);
    if (check(part == (STRPTR)buf + 5, "PathPart() preserves the original buffer"))
    {
        part[0] = 0;
        check_str("PathPart() result aliases the original path", (CONST_STRPTR)buf, "RAM:a");
    }

    /* FGets() returns the guest buffer. buflen 1 still reads one character,
       as rom/dos/fgets.c does, and buflen 0 is refused before reading. */
    fh = Open(TEXT_NAME, MODE_NEWFILE);
    if (check(fh != 0, "Open() of a text file"))
    {
        check_long("Write() of three lines", Write(fh, (APTR)TEXT_LINES, sizeof(TEXT_LINES) - 1),
                   sizeof(TEXT_LINES) - 1);
        check(Close(fh) != 0, "Close() of the text file");
    }
    fh = Open(TEXT_NAME, MODE_OLDFILE);
    if (check(fh != 0, "Open() of the text file for FGets()"))
    {
        memset(buf, 0x5A, sizeof(buf));
        check(FGets(fh, (STRPTR)buf, sizeof(buf)) == (STRPTR)buf, "FGets() returns the guest buffer");
        check_str("FGets() reads a line with its newline", (CONST_STRPTR)buf, "first line\n");
        check((UBYTE)buf[12] == 0x5A, "FGets() writes only the line and its terminator");
        memset(buf, 0x5A, sizeof(buf));
        check(FGets(fh, (STRPTR)buf, 1) == (STRPTR)buf, "FGets() with buflen 1 returns the buffer");
        check(buf[0] == 's' && buf[1] == 0 && (UBYTE)buf[2] == 0x5A,
              "FGets() with buflen 1 stores one character and the terminator");
        check(FGets(fh, (STRPTR)buf, 4) == (STRPTR)buf, "FGets() with a short buffer");
        check_str("FGets() stops after buflen - 1 characters", (CONST_STRPTR)buf, "eco");
        check((UBYTE)buf[4] == 0x5A, "FGets() leaves the byte after a short buffer");
        check(FGets(fh, (STRPTR)buf, sizeof(buf)) == (STRPTR)buf, "FGets() reads on in the line");
        check_str("FGets() continues where it stopped", (CONST_STRPTR)buf, "nd\n");
        check(FGets(fh, (STRPTR)buf, sizeof(buf)) == (STRPTR)buf, "FGets() of a last line");
        check_str("FGets() returns a last line without a newline", (CONST_STRPTR)buf, "last");
        check(FGets(fh, (STRPTR)buf, sizeof(buf)) == NULL, "FGets() returns NULL at EOF");
        part = FGets(fh, (STRPTR)buf, 0);
        error = IoErr();
        check(part == NULL, "FGets() refuses buflen 0");
        check_long("IoErr() after FGets() with buflen 0", error, ERROR_BAD_NUMBER);
        check(Close(fh) != 0, "Close() after FGets()");
    }
    check(DeleteFile(TEXT_NAME) != 0, "DeleteFile() of the text file");

    memset(&ds, 0, sizeof(ds));
    check(DateStamp(&ds) == &ds, "DateStamp() returns the original guest pointer");
    Printf("     DateStamp(): days %ld, minute %ld, tick %ld\n",
           ds.ds_Days, ds.ds_Minute, ds.ds_Tick);
    check(ds.ds_Minute >= 0 && ds.ds_Minute < 24 * 60, "DateStamp() ds_Minute is below 1440");
    check(ds.ds_Tick >= 0 && ds.ds_Tick < 60 * TICKS_PER_SECOND, "DateStamp() ds_Tick is below 3000");

    if (dfib)
        FreeDosObject(DOS_FIB, dfib);
    return test_done("dos_fileio");
}
