#pragma once
#include <exec/types.h>
#include <exec/nodes.h>
struct Task { struct Node tc_Node; APTR tc_UserData; };
struct Library { int marker; };
#define NT_PROCESS 13
