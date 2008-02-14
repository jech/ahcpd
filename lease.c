/*
Copyright (c) 2008 by Juliusz Chroboczek

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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "ahcpd.h"
#include "lease.h"

#ifdef NO_STATEFUL_SERVER

int
lease_init(const char *dir, unsigned int first, unsigned int last)
{
    return -1;
}

int
take_lease(const unsigned char *client_id, int client_id_len,
           const unsigned char *suggested_ipv4,
           unsigned char *ipv4_return, unsigned short *lease_time)
{
    return -1;
}

int
release_lease(const unsigned char *ipv4,
              const unsigned char *client_id, int client_id_len)
{
    return -1;
}

#else

#define MAX_LEASE_TIME 3600
#define LEASE_GRACE_TIME 600

static unsigned int first_address = 0, last_address = 0;
const char *lease_directory = NULL;

static char *
lease_file(const unsigned char *ipv4)
{
    char buf[255];
    const char *p;
    int n;
    strncpy(buf, lease_directory, 255);
    strncat(buf, "/", 255);

    n = strlen(buf);
    if(n >= 255)
        return NULL;

    p = inet_ntop(AF_INET, ipv4, buf + n, 254 - n);
    if(p == NULL)
        return NULL;

    return strdup(buf);
}

static int
open_lease_file(char *fn)
{
    char buf[300];
    int rc;

    if(strlen(fn) > 255)
        return -1;

    strncpy(buf, fn, 300);
    strncat(buf, ".lock", 300);

    rc = link(fn, buf);
    if(rc < 0)
        return -1;

    rc = open(fn, O_RDWR);
    if(rc < 0) {
        int save = errno;
        unlink(buf);
        errno = save;
        return -1;
    }

    return rc;
}

static int
create_lease_file(char *fn)
{
    char buft[300], buf[300];
    int fd, rc;

    if(strlen(fn) > 255)
        return -1;

    strncpy(buf, fn, 300);
    strncat(buf, ".lock", 300);

    /* O_EXCL is unsafe over NFS, do it the hard way */
    snprintf(buft, 300, "%s.%lu\n", fn, (unsigned long)getpid());
    fd = open(buft, O_RDWR | O_CREAT | O_EXCL, 0644);
    if(fd < 0) {
        perror("creat(temp_lease_file)");
        return -1;
    }
    rc = link(buft, buf);
    if(rc < 0) {
        int save = errno;
        unlink(buft);
        errno = save;
        return -1;
    }
    unlink(buft);

    rc = link(buf, fn);
    if(rc < 0) {
        int save = errno;
        unlink(buf);
        errno = save;
        return -1;
    }

    return fd;
}

static int
close_lease_file(char *fn, int fd)
{
    char buf[300];
    int rc;

    if(strlen(fn) > 255)
        return -1;

    rc = fsync(fd);
    if(fd < 0)
        return -1;

    strncpy(buf, fn, 300);
    strncat(buf, ".lock", 300);

    rc = unlink(buf);
    if(buf < 0)
        perror("unlink(lease_lock)");

    return close(fd);
}

static int
read_lease_file(int fd, const unsigned char *ipv4,
                int *lease_end_return,
                unsigned char *client_buf, int len)
{
    int rc;
    char buf[700];
    int lease_end;

    rc = read(fd, buf, 700);
    if(rc < 0) {
        perror("read(lease_file)");
        return -1;
    }

    if(rc < 16 || rc >= 700) {
        fprintf(stderr, "Truncated lease file.\n");
        return -1;
    }

    if(memcmp("AHCP\0\0\0\0", buf, 8) != 0) {
        fprintf(stderr, "Corrupted lease file.\n");
        return -1;
    }

    if(memcmp(ipv4, buf + 8, 4) != 0) {
        fprintf(stderr, "Mismatched lease file.\n");
        return -1;
    }

    memcpy(&lease_end, buf + 12, 4);
    lease_end = ntohl(lease_end);

    if(client_buf) {
        if(len < rc - 16)
            return -1;
        memcpy(client_buf, buf + 8, rc - 8);
    }
    if(lease_end_return)
        *lease_end_return = lease_end;
    return rc - 16;
}

static int
write_lease_file(int fd,
                 const unsigned char *ipv4, int lease_end,
                 const unsigned char *client_id, int client_id_len)
{
    char buf[700];
    int rc;

    if(client_id_len > 650)
        return -1;

    lease_end = htonl(lease_end);

    memcpy(buf, "AHCP", 4);
    memset(buf + 4, 0, 4);
    memcpy(buf + 8, ipv4, 4);
    memcpy(buf + 12, &lease_end, 4);
    memcpy(buf + 16, client_id, client_id_len);
    rc = write(fd, buf, 16 + client_id_len);
    if(rc < 0) {
        perror("write(lease_file)");
        return -1;
    }
    return 1;
}

static int
update_lease_file(int fd, int lease_end)
{
    off_t lrc;
    int rc;

    lease_end = htonl(lease_end);

    lrc = lseek(fd, 12, SEEK_SET);
    if(lrc < 0) {
        perror("lseek(lease_file)");
        return -1;
    }

    rc = write(fd, &lease_end, 4);
    if(rc < 0) {
        perror("write(lease_file)");
        return -1;
    }

    return 1;
}

static int
get_lease(const unsigned char *ipv4, unsigned short lease_time,
          const unsigned char *client_id, int client_id_len)
{
    unsigned char buf[700];
    char *f;
    int fd, rc;
    off_t lrc;
    int old_lease_end, lease_end;
    struct timeval now;

    gettimeofday(&now, NULL);
    lease_end = now.tv_sec + 1 + lease_time;

    f = lease_file(ipv4);
    if(f == NULL)
        return -1;

    fd = open_lease_file(f);
    if(fd < 0) {
        if(errno == ENOENT)
            goto create;
        perror("open(lease_file)");
        return -1;
    }

    rc = read_lease_file(fd, ipv4, &old_lease_end, buf, 700);
    if(rc < 0) {
        fprintf(stderr, "Couldn't read lease file.\n");
        goto fail;
    }

    if(rc == client_id_len && memcmp(buf, client_id, rc) == 0) {
        lrc = lseek(fd, 0, SEEK_SET);
        if(lrc < 0) {
            perror("lseek(lease_file)");
            goto fail;
        }

        rc = update_lease_file(fd, lease_end);
        if(rc < 0)
            goto fail;
        close_lease_file(f, fd);
        return 1;
    } else {
        if(old_lease_end + LEASE_GRACE_TIME < now.tv_sec) {
            rc = unlink(f);
            if(rc < 0) {
                perror("unlink(lease_file)");
                goto fail;
            }

            close_lease_file(f, fd);
            goto create;
        }
        goto fail;
    }

 create:
    fd = create_lease_file(f);
    if(fd < 0) {
        perror("creat(lease_file)");
        return -1;
    }

    rc = write_lease_file(fd, ipv4, lease_end, client_id, client_id_len);
    if(rc < 0)
        goto fail;

    close_lease_file(f, fd);
    return 1;

 fail:
    close_lease_file(f, fd);
    return -1;
}

int
lease_init(const char *dir, unsigned int first, unsigned int last)
{
    lease_directory = dir;
    first_address = first;
    last_address = last;
    return 1;
}

int
release_lease(const unsigned char *ipv4,
              const unsigned char *client_id, int client_id_len)
{
    unsigned char buf[700];
    char *f;
    int fd, rc;
    struct timeval now;

    if(first_address == 0 || lease_directory == NULL)
        return -1;

    f = lease_file(ipv4);
    if(f == NULL)
        return -1;

    fd = open_lease_file(f);
    if(fd < 0) {
        perror("open(lease_file)");
        return -1;
    }

    rc = read_lease_file(fd, ipv4, NULL, buf, 700);
    if(rc < 0) {
        fprintf(stderr, "Couldn't read lease file.\n");
        goto fail;
    }

    if(client_id) {
        if(rc != client_id_len || memcmp(buf, client_id, rc) != 0) {
            fprintf(stderr, "Client id mismatch.\n");
            goto fail;
        }
    }

    gettimeofday(&now, NULL);

    rc = update_lease_file(fd, now.tv_sec);
    if(rc < 0) {
        rc = unlink(f);
        if(rc < 0) {
            perror("unlink(lease_file)");
            goto fail;
        }
    }

    close_lease_file(f, fd);
    return 1;

 fail:
    close_lease_file(f, fd);
    return -1;
}

int
take_lease(const unsigned char *client_id, int client_id_len,
           const unsigned char *suggested_ipv4,
           unsigned char *ipv4_return, unsigned short *lease_time)
{
    unsigned char ipv4[4];
    int rc;
    unsigned int a, an, a0;
    unsigned short time;

    if(first_address == 0 || lease_directory == NULL)
        return -1;

    time = *lease_time;
    if(time > MAX_LEASE_TIME)
        time = MAX_LEASE_TIME;

    a0 = first_address;
    if(suggested_ipv4) {
        memcpy(&a0, suggested_ipv4, 4);
        a0 = ntohl(a0);
        if(a0 < first_address || a0 > last_address)
            a0 = first_address;
    }

    a = a0;

    do {
        if(a > last_address)
            a = first_address;

        an = htonl(a);
        memcpy(ipv4, &an, 4);
        rc = get_lease(ipv4, time, client_id, client_id_len);
        if(rc >= 0) {
            memcpy(ipv4_return, ipv4, 4);
            *lease_time = time;
            return 1;
        }
        a++;
    } while (a != a0);

    return -1;
}

#endif
