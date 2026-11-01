/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    Shared protocol constants and helpers for asl_file_probe and WaitAslWindow.
*/
#ifndef ASL_PROBE_PROTOCOL_H
#define ASL_PROBE_PROTOCOL_H

#include <stdio.h>
#include <string.h>

#define AP_NONCE "KAFASL261101P1"
/* The one display size of the emulated raspi3b framebuffer; AROS clamps a
   smaller screen up to it. */
#define AP_SCREEN_W 1024
#define AP_SCREEN_H 768
#define AP_SCREEN_DEPTH 24
#define AP_REQUEST_LEFT 60
#define AP_REQUEST_TOP 60
#define AP_REQUEST_W 520
#define AP_REQUEST_H 300
#define AP_USERDATA 0x13572468UL
#define AP_PATH 160
#define AP_TITLE 80
#define AP_POLL_TICKS 5
#define AP_POLLS 600
/* Between the screenshot and the key markers: longer than one five-second
   look of the runner. A heuristic order, proved only by the runner log. */
#define AP_SHOT_TICKS 350
#define AP_VALUES 12

enum { AP_SL, AP_ST, AP_SW, AP_SH, AP_WL, AP_WT, AP_WW, AP_WH,
       AP_BL, AP_BT, AP_BR, AP_BB };

static int ap_case(const char *name)
{
    if (!strcmp(name, "asl_accept")) return 1;
    if (!strcmp(name, "asl_cancel")) return 0;
    return -1;
}

static int ap_path(char *dst, const char *name, const char *suffix, int n)
{
    int len = snprintf(dst, AP_PATH, "SYS:EMU68/%s-%s-%d.txt", name, suffix, n);
    return len > 0 && len < AP_PATH;
}

static int ap_title(char *dst, const char *name, const char *which, int n)
{
    int len = snprintf(dst, AP_TITLE, "%s-%s-%d-%s", AP_NONCE, name, n, which);
    return len > 0 && len < AP_TITLE;
}

#endif
