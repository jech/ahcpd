/*
Copyright (c) 2006 by Juliusz Chroboczek

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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

int
main(int argc, char **argv)
{
    char *sprefix = NULL, *smac = NULL;
    const char *p;
    unsigned char mac[6];
    unsigned char address[16];
    char saddress[100];
    int i, rc, fd;
    int random_prefix = 0, print_prefix = 0;

    const char *usage =
        "Usage: ahcp-generate-address [-p] {-r | prefix} [mac-48]\n";

    i = 1;
    while(i < argc) {
        if(argv[i][0] != '-') {
            break;
        } else if(strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if(strcmp(argv[i], "-p") == 0) {
            print_prefix = 1;
            i++;
        } else if(strcmp(argv[i], "-r") == 0) {
            random_prefix = 1;
            i++;
        } else {
            goto usage;
        }
    }

    if(!random_prefix) {
        if(i >= argc)
            goto usage;
        sprefix = argv[i];
        i++;
    }

    if(i < argc) {
        smac = argv[i];
        i++;
    } else {
        smac = NULL;
    }

    if(i < argc)
        goto usage;

    if(!smac || random_prefix) {
        fd = open("/dev/urandom", O_RDONLY);
        if(fd < 0) {
            perror("open(random)");
            exit(1);
        }
    } else {
        fd = -1;
    }

    if(random_prefix) {
        address[0] = (0xFC | 0x01); /* locally generated */
        rc = read(fd, address + 1, 5);
        if(rc != 5) {
            perror("read(random)");
            exit(1);
        }
        memset(address + 6, 0, 2);
    } else {
        rc = inet_pton(AF_INET6, sprefix, address);
        if(rc != 1)
            goto usage;
    }

    if(print_prefix) {
        memset(address + 8, 0, 8);
    } else if(smac) {
        rc = sscanf(smac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                    &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        if(rc != 6)
            goto usage;

        if((mac[0] & 1) != 0) {
            fprintf(stderr, "Warning: group bit is not 0.\n");
            mac[0] &= ~1;
        }

        address[8] = mac[0] ^ 2;
        address[9] = mac[1];
        address[10] = mac[2];
        address[11] = 0xFF;
        address[12] = 0xFE;
        address[13] = mac[3];
        address[14] = mac[4];
        address[15] = mac[5];
    } else {
        rc = read(fd, address + 8, 8);
        if(rc != 8) {
            perror("read(random)");
            exit(1);
        }
        address[8] &= ~3;
    }

    if(fd >= 0)
        close(fd);

    p = inet_ntop(AF_INET6, address, saddress, 100);
    if(p == NULL) {
        perror("inet_ntop");
        exit(1);
    }

    printf("%s\n", p);
    return 0;

 usage:
    fputs(usage, stderr);
    exit(1);
}
