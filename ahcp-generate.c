/*
Copyright (c) 2006-2007 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#if defined(__APPLE__)
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define bswap_64 OSSwapInt64
#else
#include <endian.h>
#include <byteswap.h>
#endif
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "config.h"
#include "constants.h"

#define MAXSIZE 2048

unsigned int expires_delta = 4 * 30 * 24 * 60 * 60;
char *prefix = NULL;
unsigned char routing_protocol = ROUTING_PROTOCOL_BABEL;
char *default_gateway = NULL;
char *name_server = NULL;
unsigned char olsr_multicast_address[16] =
    { 0xff, 0x04, 0, 0, 0, 0, 0, 0,
      0xcc, 0xa6, 0xc0, 0xf9, 0xe1, 0x82, 0x53, 0x59 };
unsigned char babel_multicast_address[16] =
    { 0xff, 0x02, 0, 0, 0, 0, 0, 0,
      0xcc, 0xa6, 0xc0, 0xf9, 0xe1, 0x82, 0x53, 0x73 };
unsigned int babel_port_number = 8475;
unsigned char link_quality = 0;

int
main(int argc, char **argv)
{
    unsigned char buf[MAXSIZE];
    union {
        unsigned short s;
        char c[2];
    } twobytes;
    union {
        unsigned int i;
        char c[4];
    } fourbytes;
    int i;
    char *usage =
        "ahcp-generate -p prefix [-P protocol] [-g gw] [-n ns] [-e seconds] > ahcp.dat";
    int rc;

    while(1) {
        int c;
        c = getopt(argc, argv, "p:n:g:P:e:");
        if(c < 0) break;
        switch(c) {
        case 'p': prefix = optarg; break;
        case 'n': name_server = optarg; break;
        case 'g': default_gateway = optarg; break;
        case 'P':
            if(strcasecmp(optarg, "static") == 0)
                routing_protocol = ROUTING_PROTOCOL_STATIC;
            else if(strcasecmp(optarg, "olsr") == 0)
                routing_protocol = ROUTING_PROTOCOL_OLSR;
            else if(strcasecmp(optarg, "babel") == 0)
                routing_protocol = ROUTING_PROTOCOL_BABEL;
            else {
                fprintf(stderr, "Unknown routing protocol %s.\n", optarg);
                exit(1);
            }
            break;
        case 'e': expires_delta = atoi(optarg); break;
        default: fprintf(stderr, "%s\n", usage); exit(1); break;
        }
    }

    if(prefix == NULL) {
        fprintf(stderr, "%s\n", usage);
        exit(1);
    }

    if(argc != optind) {
        fprintf(stderr, "%s\n", usage); exit(1);
    }

    i = 0;

#define EMIT(a, n) if(i > MAXSIZE - n) goto fail; memcpy(buf + i, a, n); i += n;

#define EMIT1(b) if(i > MAXSIZE - 1) goto fail; buf[i++] = b;

#define EMIT2(ss) do { \
    if(i > MAXSIZE - 2) goto fail; \
    twobytes.s = htons(ss); \
    buf[i++] = twobytes.c[0]; \
    buf[i++] = twobytes.c[1]; } while(0)

#define EMIT4(n) do {               \
    if(i > MAXSIZE - 4) goto fail; \
    fourbytes.i = htonl(n); \
    buf[i++] = fourbytes.c[0]; \
    buf[i++] = fourbytes.c[1]; \
    buf[i++] = fourbytes.c[2]; \
    buf[i++] = fourbytes.c[3]; } while (0)

#define EMIT8(l) do { \
    if(i > MAXSIZE - 8) goto fail; \
    eightbytes.i = htonll(l); \
    buf[i++] = eightbytes.c[0]; \
    buf[i++] = eightbytes.c[1]; \
    buf[i++] = eightbytes.c[2]; \
    buf[i++] = eightbytes.c[3]; \
    buf[i++] = eightbytes.c[4]; \
    buf[i++] = eightbytes.c[5]; \
    buf[i++] = eightbytes.c[6]; \
    buf[i++] = eightbytes.c[7]; } while(0)

    if(expires_delta > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_EXPIRES);
        EMIT1(4);
        EMIT4(now.tv_sec + expires_delta);
    }

    if(prefix) {
        unsigned char p[16];
        rc = inet_pton(AF_INET6, prefix, p);
        if(rc < 0) {
            fprintf(stderr, "Couldn't parse prefix.\n");
            exit(1);
        }
        EMIT1(OPT_IPv6_PREFIX);
        EMIT1(16);
        EMIT(p, 16);
    }

    if(routing_protocol == ROUTING_PROTOCOL_STATIC) {
        unsigned char g[16];
        if(default_gateway) {
            rc = inet_pton(AF_INET6, default_gateway, g);
            if(rc < 0) {
                fprintf(stderr, "Couldn't parse default gateway.\n");
                exit(1);
            }
        }
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_ROUTING_PROTOCOL);
        EMIT1(default_gateway ? 19 : 1);
        EMIT1(ROUTING_PROTOCOL_STATIC);
        if(default_gateway) {
            EMIT1(STATIC_DEFAULT_GATEWAY);
            EMIT1(16);
            EMIT(g, 16);
        }
    } else if(routing_protocol == ROUTING_PROTOCOL_OLSR) {
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_ROUTING_PROTOCOL);
        EMIT1(link_quality ? 23 : 19);
        EMIT1(ROUTING_PROTOCOL_OLSR);
        EMIT1(OLSR_MULTICAST_ADDRESS);
        EMIT1(16);
        EMIT(olsr_multicast_address, 16);
        if(link_quality) {
            EMIT1(OPT_MANDATORY);
            EMIT1(OLSR_LINK_QUALITY);
            EMIT1(1);
            EMIT1(link_quality);
        }
    } else if(routing_protocol == ROUTING_PROTOCOL_BABEL) {
        EMIT1(OPT_MANDATORY);
        EMIT1(OPT_ROUTING_PROTOCOL);
        EMIT1(23);
        EMIT1(ROUTING_PROTOCOL_BABEL)
        EMIT1(BABEL_MULTICAST_ADDRESS);
        EMIT1(16);
        EMIT(babel_multicast_address, 16);
        EMIT1(BABEL_PORT_NUMBER);
        EMIT1(2);
        EMIT2(babel_port_number);
    }

    if(name_server) {
        unsigned char n[16];
        rc = inet_pton(AF_INET6, name_server, n);
        if(rc < 0) {
            fprintf(stderr, "Couldn't parse name server.\n");
            exit(1);
        }
        EMIT1(OPT_NAME_SERVER);
        EMIT1(16);
        EMIT(n, 16);
    }
    write(1, buf, i);
    return 0;

 fail:
    fprintf(stderr, "Buffer overflow");
    exit(1);
}
