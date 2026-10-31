/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Optional host emitter for structural/disassembly review. It reads no
   executable; the native controller uses the identical original builder. */
#include "domain_fixture_hunks.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    static uint8_t image[32768];
    char path[1024], name[96], proof[160];
    const char *backend;
    size_t n;
    FILE *f;
    if (argc != 3 || (strcmp(argv[2], "moira") && strcmp(argv[2], "jit")))
    {
        fprintf(stderr, "usage: domain_fixture_emit OUTPUT_DIRECTORY moira|jit\n");
        return 2;
    }
    backend = argv[2];
    snprintf(name, sizeof(name), "kaffeine-domain-child-%s", backend);
    snprintf(proof, sizeof(proof), "RAM:kaffeine-domain-%s/child/proof", backend);
    n = df_build_root(image, sizeof(image), name);
    snprintf(path, sizeof(path), "%s/domain_root", argv[1]);
    f = n ? fopen(path, "wb") : NULL;
    if (!f) return 1;
    if (fwrite(image, 1, n, f) != n || fclose(f)) return 1;
    printf("original root: %zu bytes\n", n);
    n = df_build_child(image, sizeof(image), backend, proof);
    snprintf(path, sizeof(path), "%s/domain_child", argv[1]);
    f = n ? fopen(path, "wb") : NULL;
    if (!f) return 1;
    if (fwrite(image, 1, n, f) != n || fclose(f)) return 1;
    printf("original child: %zu bytes\n", n);
    return 0;
}
