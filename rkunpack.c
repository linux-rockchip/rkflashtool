/*
 * Copyright (c) 2010 Fukaumi Naoki
 * Copyright (c) 2013 Ivo van Poorten
 * Copyright (c) 2020 Hajo Noerenberg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "version.h"

#ifdef _WIN32       /* hack around non-posix behaviour */
#undef mkdir
#define mkdir(a,b) _mkdir(a)
int _mkdir(const char *);
#include <windows.h>
HANDLE fm;
#else
#define O_BINARY 0
#endif

static uint8_t *buf;
static off_t size;
static unsigned int fsize, ioff, isize, noff;
static int fd;

static const char *const strings[2] = { "info", "fatal" };

static void info_and_fatal(const int s, const char *f, ...) {
    va_list ap;
    va_start(ap,f);
    fprintf(stderr, "rkunpack: %s: ", strings[s]);
    vfprintf(stderr, f, ap);
    va_end(ap);
    if (s) exit(s);
}

#define info(...)   info_and_fatal(0, __VA_ARGS__)
#define fatal(...)  info_and_fatal(1, __VA_ARGS__)

#define GET32LE(x) ((x)[0] | (x)[1] << 8 | (x)[2] << 16 | (x)[3] << 24)

static void write_file(const char *path, uint8_t *buffer, unsigned int length) {
    int img;
    if ((img = open(path, O_BINARY | O_WRONLY | O_CREAT | O_TRUNC, 0644)) == -1 ||
               write(img, buffer, length) == -1 ||
               close(img) == -1)
        fatal("%s: %s\n", path, strerror(errno));
}

static void unpack_rkaf(void) {
    uint8_t *p;
    const char *name, *path, *sep;
    char dir[PATH_MAX];
    int count;

    info("RKAF signature detected\n");

    fsize = GET32LE(buf+4) + 4;
    if (fsize != (unsigned)size)
        info("invalid file size (should be %u bytes)\n", fsize);
    else
        info("file size matches (%u bytes)\n", fsize);

    info("manufacturer: %s\n", buf + 0x48);
    info("model: %s\n", buf + 0x08);

    count = GET32LE(buf+0x88);

    info("number of files: %d\n", count);

    for (p = &buf[0x8c]; count > 0; p += 0x70, count--) {
        name = (const char *)p;
        path = (const char *)p + 0x20;

        ioff  = GET32LE(p+0x60);
        noff  = GET32LE(p+0x64);
        isize = GET32LE(p+0x68);
        fsize = GET32LE(p+0x6c);

        if (memcmp(path, "SELF", 4) == 0) {
            info("skipping SELF entry\n");
        } else {
            info("%08x-%08x %-26s (size: %u)\n", ioff, ioff + isize - 1, path, fsize);

            // strip header and footer of parameter file
            if (memcmp(name, "parameter", 9) == 0) {
                ioff += 8;
                fsize -= 12;
            }

            sep = path;
            while ((sep = strchr(sep, '/')) != NULL) {
                memcpy(dir, path, sep - path);
                dir[sep - path] = '\0';
                if (mkdir(dir, 0755) == -1 && errno != EEXIST)
                    fatal("%s: %s\n", dir, strerror(errno));
                sep++;
            }

            write_file(path, buf+ioff, fsize);
        }
    }
}

static void unpack_rkfw(void) {
    const char *chip = NULL;
    char *first_part = NULL;

    info("RKFW signature detected\n");
    info("version: %d.%d.%d\n", buf[9], buf[8], (buf[7]<<8)+buf[6]);
    info("date: %d-%02d-%02d %02d:%02d:%02d\n",
            (buf[0x0f]<<8)+buf[0x0e], buf[0x10], buf[0x11],
            buf[0x12], buf[0x13], buf[0x14]);
    switch(buf[0x15]) {
    case 0x50:  chip = "rk29xx"; break;
    case 0x60:  chip = "rk30xx"; break;
    case 0x70:  chip = "rk31xx"; break;
    case 0x80:  chip = "rk32xx"; break;
    case 0x41:  chip = "rk3368"; break;
    case 0x36:  chip = "rv1106"; break;
    case 0x38:  chip = "rk35xx"; break;
    default: info("You got a brand new chip (%#x), congratulations!!!\n", buf[0x15]);
    }
    info("family: %s\n", chip ? chip : "unknown");

    ioff  = GET32LE(buf+0x19);
    isize = GET32LE(buf+0x1d);

    if (!memcmp(buf+ioff, "BOOT", 4)) {
        first_part = "BOOT";
    } else if (!memcmp(buf+ioff, "LDR ", 4)) {
        first_part = "LDR";
    } else {
        fatal("cannot find BOOT signature\n");
    }

    info("%08x-%08x %-26s (size: %u)\n", ioff, ioff + isize -1, first_part, isize);
    write_file(first_part, buf+ioff, isize);

    ioff  = GET32LE(buf+0x21);
    isize = GET32LE(buf+0x25);

    if (memcmp(buf+ioff, "RKAF", 4))
        fatal("cannot find embedded RKAF update.img\n");

    info("%08x-%08x %-26s (size: %u)\n", ioff, ioff + isize -1, "embedded-update.img", isize);
    write_file("embedded-update.img", buf+ioff, isize);

}

static void unpack_rkfp(void) {
    uint8_t *p;
    unsigned int pss, peo, pbeo, pes, pec;
    const char *path;
    int count;

    info("RKFP signature detected\n");
    info("version: %d.%d.%d\n", buf[15], buf[14], (buf[13]<<8)+buf[12]);
    info("date: %d-%02d-%02d %02d:%02d:%02d\n",
            (buf[0x05]<<8)+buf[0x04], buf[0x06], buf[0x07],
            buf[0x08], buf[0x09], buf[0x0a]);

    pss = GET32LE(buf+0x10);
    peo = GET32LE(buf+0x14);
    pbeo = GET32LE(buf+0x18);
    pes = GET32LE(buf+0x1c);
    pec = GET32LE(buf+0x20);

    info("partition sector size: %d bytes\n", pss);
    info("partition entry offset: %d sectors, backup partition entry offset: %d sectors\n", peo, pbeo);
    info("partition entry size: %d bytes\n", pes);
    info("partition entry count: %d\n", pec);
    info("fw size: %d\n", GET32LE(buf+0x24));
    info("partition entry crc: %08x\n", GET32LE(buf+504));
    info("header crc: %08x\n", GET32LE(buf+508));

    for (count = 1; count <= GET32LE(buf+0x20); count++) {

        p = &buf[pss*peo+(count-1)*pes];
        path = (const char *)p;
        ioff  = GET32LE(p+36);
        isize = GET32LE(p+40);
        fsize = GET32LE(p+44);

        info("%08x-%08x %-26s (type: %02x) (property: %02x) (size: %u)\n",
            ioff*pss, (ioff + isize)*pss, path, GET32LE(p+32), GET32LE(p+48), fsize);
        write_file(path, buf+(ioff*pss), fsize);
    }

}

int main(int argc, char *argv[]) {

    if (argc != 2)
        fatal("rkunpack v%d.%d\nusage: %s update.img\n",
               RKFLASHTOOL_VERSION_MAJOR,
               RKFLASHTOOL_VERSION_MINOR, argv[0]);

    if ((fd = open(argv[1], O_BINARY | O_RDONLY)) == -1)
        fatal("%s: %s\n", argv[1], strerror(errno));

    if ((size = lseek(fd, 0, SEEK_END)) == -1)
        fatal("%s: %s\n", argv[1], strerror(errno));

#ifdef _WIN32
    fm  = CreateFileMapping((HANDLE)_get_osfhandle(fd), NULL, PAGE_READONLY, 0, 0, NULL);
    buf = MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
    if (!buf) fatal("%s: cannot create MapView of File\n", argv[1]);
#else
    if ((buf = mmap(NULL, size, PROT_READ, MAP_SHARED | MAP_FILE, fd, 0))
                                                        == MAP_FAILED)
        fatal("%s: %s\n", argv[1], strerror(errno));
#endif

         if (!memcmp(buf, "RKAF", 4)) unpack_rkaf();
    else if (!memcmp(buf, "RKFW", 4)) unpack_rkfw();
    else if (!memcmp(buf, "RKFP", 4)) unpack_rkfp();
    else fatal("%s: invalid signature\n", argv[1]);

    printf("unpacked\n");

#ifdef _WIN32
    CloseHandle(fm);
    UnmapViewOfFile(buf);
#else
    munmap(buf, size);
#endif

    close(fd);

    return 0;
}
