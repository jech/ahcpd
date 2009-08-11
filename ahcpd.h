/*
Copyright (c) 2007-2009 by Juliusz Chroboczek

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

#undef MAX
#undef MIN

#define MAX(x,y) ((x)<=(y)?(y):(x))
#define MIN(x,y) ((x)<=(y)?(x):(y))

extern int nodns, af;
extern char *config_script;
extern int debug_level;
extern unsigned char myid[8];
extern int numnetworks;
struct network {
    char *ifname;
    int ifindex;
};

#define MAXNETWORKS 20
extern struct network networks[MAXNETWORKS];

extern struct in6_addr protocol_group;
extern unsigned int protocol_port;
extern int protocol_socket;

extern const unsigned char zeroes[16], ones[16];

void timeval_min(struct timeval *d, const struct timeval *s);
void timeval_min_sec(struct timeval *d, int secs);
void timeval_minus(struct timeval *d,
                   const struct timeval *s1, const struct timeval *s2);
int timeval_minus_msec(const struct timeval *s1, const struct timeval *s2);
void timeval_plus_msec(struct timeval *d,
                       const struct timeval *s, int msecs);
int timeval_compare(const struct timeval *s1, const struct timeval *s2);
int clock_stepped();
