#pragma once
#include <exec/tasks.h>
#include <dos/bptr.h>
struct Process { struct Task pr_Task; char *pr_Arguments; BPTR pr_HomeDir, pr_CurrentDir, pr_CIS, pr_COS, pr_CES, pr_CLI; };
