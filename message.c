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
#include <arpa/inet.h>

#include "message.h"

int
validate_packet(unsigned char *buf, int len)
{
    if(len < 4)
        return 0;

    if(buf[0] != 43)
        return 0;

    if(buf[1] != 0)
        return 0;

    return 1;
}

int
parse_reply(unsigned char *buf, int len,
            unsigned int *origin_r, unsigned int *expires_r,
            unsigned short *age_r,
            unsigned char **data_r, unsigned short *dlen_r)
{
    unsigned int origin, expires;
    unsigned short age, dlen;

    if(len < 20)
        return -1;

    memcpy(&origin, buf + 4, 4);
    origin = ntohl(origin);
    memcpy(&expires, buf + 8, 4);
    expires = ntohl(expires);
    memcpy(&age, buf + 16, 2);
    age = ntohs(age);
    memcpy(&dlen, buf + 18, 2);
    dlen = ntohs(dlen);

    if(len < dlen + 20)
        return -1;

    *origin_r = origin;
    *expires_r = expires;
    *age_r = age;
    *data_r = buf + 20;
    *dlen_r = dlen;

    return 1;
}

int
parse_stateful_packet(unsigned char *buf, int len,
                      unsigned short *lease_time_r,
                      unsigned char **uid_r, unsigned short *ulen_r,
                      unsigned char **data_r, unsigned short *dlen_r)
{
    unsigned short lease_time, ulen, dlen;
    unsigned char *uid, *data;

    if(len < 8)
        return -1;

    memcpy(&lease_time, buf + 4, 2);
    lease_time = ntohs(lease_time);

    memcpy(&ulen, buf + 6, 2);
    ulen = ntohs(ulen);

    if(ulen > 500)
        return -1;

    if(len < 8 + ulen)
        return -1;

    uid = buf + 8;

    if(len < 8 + ulen + 2) {
        data = NULL;
        dlen = 0;
    } else {
        memcpy(&dlen, buf + 8 + ulen, 2);
        dlen = ntohs(dlen);

        if(len < 8 + ulen + 2 + dlen)
            return -1;
        data = buf + 8 + ulen + 2;
    }

    *lease_time_r = lease_time;
    *uid_r = uid;
    *ulen_r = ulen;
    *data_r = data;
    *dlen_r = dlen;
    return 1;
}
