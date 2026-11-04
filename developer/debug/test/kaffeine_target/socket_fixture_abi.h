/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1)
   Independent literal AmiTCP v4 ABI fixture, using the standard NDK's call
   macros. No TCP SDK headers or native bridge declarations are imported. */
#ifndef SOCKET_FIXTURE_ABI_H
#define SOCKET_FIXTURE_ABI_H
#include <exec/types.h>
#include <inline/macros.h>
#include <string.h>

struct KSocketAddress { UBYTE bytes[16]; };
struct KSocketIn {
    UBYTE sin_len, sin_family;
    UWORD sin_port;
    struct { ULONG s_addr; } sin_addr;
    UBYTE padding[8];
};
struct KSocketTime { ULONG secs, micros; };
typedef struct { ULONG words[8]; } KSocketSet;
#define KFD_ZERO(s) memset((s), 0, sizeof(*(s)))
#define KFD_SET(fd,s) ((s)->words[(fd)/32] |= 1UL << ((fd)%32))
#define KFD_ISSET(fd,s) (((s)->words[(fd)/32] >> ((fd)%32)) & 1)
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 4
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define INADDR_LOOPBACK 0x7f000001UL
#define FIONBIO 0x8004667eUL
#define EWOULDBLOCK 35
#define EFAULT 14
#define EINTR 4
#define htons(v) (v)
#define htonl(v) (v)

#define socket(d,t,p) LP3(30,LONG,socket,LONG,d,d0,LONG,t,d1,LONG,p,d2,,SocketBase)
#define bind(s,a,n) LP3(36,LONG,bind,LONG,s,d0,const struct KSocketAddress *,a,a0,LONG,n,d1,,SocketBase)
#define listen(s,b) LP2(42,LONG,listen,LONG,s,d0,LONG,b,d1,,SocketBase)
#define accept(s,a,n) LP3(48,LONG,accept,LONG,s,d0,struct KSocketAddress *,a,a0,LONG *,n,a1,,SocketBase)
#define connect(s,a,n) LP3(54,LONG,connect,LONG,s,d0,const struct KSocketAddress *,a,a0,LONG,n,d1,,SocketBase)
#define send(s,p,n,f) LP4(66,LONG,send,LONG,s,d0,CONST_APTR,p,a0,LONG,n,d1,LONG,f,d2,,SocketBase)
#define recv(s,p,n,f) LP4(78,LONG,recv,LONG,s,d0,APTR,p,a0,LONG,n,d1,LONG,f,d2,,SocketBase)
#define setsockopt(s,l,o,p,n) LP5(90,LONG,setsockopt,LONG,s,d0,LONG,l,d1,LONG,o,d2,CONST_APTR,p,a0,LONG,n,d3,,SocketBase)
#define getsockname(s,a,n) LP3(102,LONG,getsockname,LONG,s,d0,struct KSocketAddress *,a,a0,LONG *,n,a1,,SocketBase)
#define IoctlSocket(s,r,p) LP3(114,LONG,IoctlSocket,LONG,s,d0,ULONG,r,d1,APTR,p,a0,,SocketBase)
#define CloseSocket(s) LP1(120,LONG,CloseSocket,LONG,s,d0,,SocketBase)
#define WaitSelect(n,r,w,e,t,s) LP6(126,LONG,WaitSelect,LONG,n,d0,KSocketSet *,r,a0,KSocketSet *,w,a1,KSocketSet *,e,a2,struct KSocketTime *,t,a3,ULONG *,s,d1,,SocketBase)
#define ObtainSocket(i,d,t,p) LP4(144,LONG,ObtainSocket,LONG,i,d0,LONG,d,d1,LONG,t,d2,LONG,p,d3,,SocketBase)
#define ReleaseSocket(s,i) LP2(150,LONG,ReleaseSocket,LONG,s,d0,LONG,i,d1,,SocketBase)
#define Errno() LP0(162,LONG,Errno,,SocketBase)
#endif
