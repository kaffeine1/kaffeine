/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Original 68k payload for the native hunk_process_native companion.
   Relative files and a private-data barrier exercise separate DOS owners. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

static volatile ULONG private_word;
static UBYTE private_bytes[56];

static BOOL write_file(CONST_STRPTR name, const void *bytes, LONG size)
{
    BPTR file = Open(name, MODE_NEWFILE);
    LONG count;
    BOOL closed;
    if (!file) return FALSE;
    count = Write(file, bytes, size);
    closed = Close(file);
    return count == size && closed;
}

int main(int argc, char **argv)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    UBYTE record[64], input[8];
    char backend[16] = {0};
    char expected_arguments[32];
    BPTR file;
    ULONG wanted;
    unsigned slot, i;
    LONG count;

    if (argc != 3 || !argv[1] || argv[1][1] ||
        (argv[1][0] != '0' && argv[1][0] != '1')) return 10;
    slot = argv[1][0] - '0';
    if (!me || SysBase->ThisTask != &me->pr_Task || !Cli() ||
        Cli() != BADDR(me->pr_CLI) || !Input() || !Output()) return 11;
    if (GetVar("KAFFEINE_CPU", backend, sizeof(backend) - 1, 0) < 0 ||
        strcmp(backend, argv[2])) return 12;
    file = Open("input", MODE_OLDFILE);
    if (!file) return 13;
    count = Read(file, input, sizeof(input));
    if (!Close(file)) return 14;
    if (count != 7 || memcmp(input, "input", 5) ||
        input[5] != '0' + slot || input[6] != '\n') return 14;

    wanted = 0xC0010000UL + slot;
    private_word = wanted;
    memset(private_bytes, 0x30 + slot, sizeof(private_bytes));
    record[0] = (UBYTE)slot;
    if (!write_file("ready", record, 1)) return 15;
    for (i = 0; i < 1500; ++i)
    {
        file = Open("release", MODE_OLDFILE);
        if (file) { if (!Close(file)) return 16; break; }
        Delay(1);
    }
    if (i == 1500 || private_word != wanted) return 16;
    for (i = 0; i < sizeof(private_bytes); ++i)
        if (private_bytes[i] != 0x30 + slot) return 17;
    expected_arguments[0] = '0' + slot;
    expected_arguments[1] = ' ';
    strcpy(expected_arguments + 2, backend);
    if (argv[1][0] != '0' + slot || argv[1][1] || strcmp(argv[2], backend) ||
        !GetArgStr() || strcmp(GetArgStr(), expected_arguments)) return 19;

    memset(record, 0, sizeof(record));
    memcpy(record, "KHF1", 4);
    record[4] = (UBYTE)slot;
    record[5] = 0x1F; /* Process/CLI, backend, directory input, private data, args */
    memcpy(record + 8, private_bytes, sizeof(private_bytes));
    if (!write_file("result", record, sizeof(record))) return 18;
    return 7 + slot;
}
