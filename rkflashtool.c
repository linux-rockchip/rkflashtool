/* rkflashtool - for RockChip based devices.
 *               (RK2808, RK2818, RK2918, RK2928, RK3066, RK3068, RK3126 and RK3188)
 *
 * Copyright (C) 2010-2014 by Ivo van Poorten, Fukaumi Naoki, Guenter Knauf,
 *                            Ulrich Prinz, Steve Wilson
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <libusb.h>

/* hack to set binary mode for stdin / stdout on Windows */
#ifdef _WIN32
#include <fcntl.h>
int _CRT_fmode = _O_BINARY;
#endif

#include "version.h"
#include "rkcrc.h"
#include "rkflashtool.h"

#define RKFT_BLOCKSIZE      0x4000      /* must be multiple of 512 */
#define RKFT_IDB_DATASIZE   0x200
#define RKFT_IDB_BLOCKSIZE  0x210
#define RKFT_IDB_INCR       0x20
#define RKFT_MEM_INCR       0x80
#define RKFT_OFF_INCR       (RKFT_BLOCKSIZE>>9)
#define MAX_PARAM_LENGTH    (128*512-12) /* cf. MAX_LOADER_PARAM in rkloader */

#define RKFT_CMD_TESTUNITREADY      0x80000600
#define RKFT_CMD_READFLASHID        0x80000601
#define RKFT_CMD_READFLASHINFO      0x8000061a
#define RKFT_CMD_READCHIPINFO       0x8000061b
#define RKFT_CMD_READEFUSE          0x80000620

#define RKFT_CMD_SETDEVICEINFO      0x00000602
#define RKFT_CMD_ERASESYSTEMDISK    0x00000616
#define RKFT_CMD_SETRESETFLASG      0x0000061e
#define RKFT_CMD_RESETDEVICE        0x000006ff

#define RKFT_CMD_TESTBADBLOCK       0x80000a03
#define RKFT_CMD_READSECTOR         0x80000a04
#define RKFT_CMD_READLBA            0x80000a14
#define RKFT_CMD_READSDRAM          0x80000a17
#define RKFT_CMD_UNKNOWN1           0x80000a21

#define RKFT_CMD_WRITESECTOR        0x00000a05
#define RKFT_CMD_ERASESECTORS       0x00000a06
#define RKFT_CMD_UNKNOWN2           0x00000a0b
#define RKFT_CMD_WRITELBA           0x00000a15
#define RKFT_CMD_WRITESDRAM         0x00000a18
#define RKFT_CMD_EXECUTESDRAM       0x00000a19
#define RKFT_CMD_WRITEEFUSE         0x00000a1f
#define RKFT_CMD_UNKNOWN3           0x00000a22

#define RKFT_CMD_WRITESPARE         0x80001007
#define RKFT_CMD_READSPARE          0x80001008

#define RKFT_CMD_LOWERFORMAT        0x0000001c
#define RKFT_CMD_WRITENKB           0x00000030

#define SETBE16(a, v) do { \
                        ((uint8_t*)a)[1] =  v      & 0xff; \
                        ((uint8_t*)a)[0] = (v>>8 ) & 0xff; \
                      } while(0)

#define SETBE32(a, v) do { \
                        ((uint8_t*)a)[3] =  v      & 0xff; \
                        ((uint8_t*)a)[2] = (v>>8 ) & 0xff; \
                        ((uint8_t*)a)[1] = (v>>16) & 0xff; \
                        ((uint8_t*)a)[0] = (v>>24) & 0xff; \
                      } while(0)

static const struct t_pid {
    const uint16_t pid;
    const char name[8];
} pidtab[] = {
    { 0x281a, "RK2818" },
    { 0x290a, "RK2918" },
    { 0x292a, "RK2928" },
    { 0x292c, "RK3026" },
    { 0x300a, "RK3066" },
    { 0x300b, "RK3168" },
    { 0x301a, "RK3036" },
    { 0x310a, "RK3066B" },
    { 0x310b, "RK3188" },
    { 0x310c, "RK312X" }, // Both RK3126 and RK3128
    { 0x310d, "RK3126" },
    { 0x320a, "RK3288" },
    { 0x320b, "RK322X" }, // Both RK3228 and RK3229
    { 0x320c, "RK3328" },
    { 0x330a, "RK3368" },
    { 0x330c, "RK3399" },
    { 0x350b, "RK3588" }, // RK3588, RK3588S, RK3588S2, RK3582
    { 0x350c, "RK3528" },
    { 0, "" },
};

typedef struct {
    uint32_t flash_size;
    uint16_t block_size;
    uint8_t page_size;
    uint8_t ecc_bits;
    uint8_t access_time;
    uint8_t manufacturer_id;
    uint8_t chip_select;
} nand_info;

static const char* const manufacturer[] = {   /* NAND Manufacturers */
    "Samsung",
    "Toshiba",
    "Hynix",
    "Infineon",
    "Micron",
    "Renesas",
    "Intel",
    "UNKNOWN", /* Reserved */
    "SanDisk",
};
#define MAX_NAND_ID (sizeof manufacturer / sizeof(char *))

static uint8_t cmd[31], res[13], buf[RKFT_BLOCKSIZE];
static uint8_t ibuf[RKFT_IDB_BLOCKSIZE];
static libusb_context *c;
static libusb_device_handle *h = NULL;
static int ep;
static int tmp;

static const char *const strings[2] = { "info", "fatal" };

static void info_and_fatal(const int s, const int cr, char *f, ...) {
    va_list ap;
    va_start(ap,f);
    fprintf(stderr, "%srkflashtool: %s: ", cr ? "\r" : "", strings[s]);
    vfprintf(stderr, f, ap);
    va_end(ap);
    if (s) exit(s);
}

#define info(...)    info_and_fatal(0, 0, __VA_ARGS__)
#define infocr(...)  info_and_fatal(0, 1, __VA_ARGS__)
#define fatal(...)   info_and_fatal(1, 0, __VA_ARGS__)

static void usage(void) {
    fatal("usage:\n"
          "\trkflashtool b [flag]            \treboot device\n"
          "\trkflashtool l <file             \tload DDR init (MASK ROM MODE)\n"
          "\trkflashtool L <file             \tload USB loader (MASK ROM MODE)\n"
          "\trkflashtool v                   \tread chip version\n"
          "\trkflashtool n                   \tread NAND flash info\n"
          "\trkflashtool i offset nsectors >outfile \tread IDBlocks\n"
          "\trkflashtool j offset nsectors <infile  \twrite IDBlocks\n"
          "\trkflashtool m offset nbytes   >outfile \tread SDRAM\n"
          "\trkflashtool M offset nbytes   <infile  \twrite SDRAM\n"
          "\trkflashtool B krnl_addr parm_addr      \texec SDRAM\n"
          "\trkflashtool r partname >outfile \tread flash partition\n"
          "\trkflashtool w partname <infile  \twrite flash partition\n"
          "\trkflashtool r offset nsectors >outfile \tread flash\n"
          "\trkflashtool w offset nsectors <infile  \twrite flash\n"
//          "\trkflashtool f                 >outfile \tread fuses\n"
//          "\trkflashtool g                 <infile  \twrite fuses\n"
          "\trkflashtool p >file             \tfetch parameters\n"
          "\trkflashtool P <file             \twrite parameters\n"
          "\trkflashtool e partname          \terase flash (fill with 0xff)\n"
          "\trkflashtool e offset nsectors   \terase flash (fill with 0xff)\n"
         );
}

static void send_exec(uint32_t krnl_addr, uint32_t parm_addr) {
    long int r = random();

    memset(cmd, 0 , 31);
    memcpy(cmd, "USBC", 4);

    if (r)
        SETBE32(cmd+4, r);
    if (krnl_addr)
        SETBE32(cmd+17, krnl_addr);
    if (parm_addr)
        SETBE32(cmd+22, parm_addr);

    SETBE32(cmd+12, RKFT_CMD_EXECUTESDRAM);

    libusb_bulk_transfer(h, ep|LIBUSB_ENDPOINT_OUT, cmd, sizeof(cmd), &tmp, 0);
}

static void send_reset(uint8_t flag) {
    long int r = random();

    memset(cmd, 0 , 31);
    memcpy(cmd, "USBC", 4);

    SETBE32(cmd+4, r);
    SETBE32(cmd+12, RKFT_CMD_RESETDEVICE);
    cmd[16] = flag;

    libusb_bulk_transfer(h, ep|LIBUSB_ENDPOINT_OUT, cmd, sizeof(cmd), &tmp, 0);
}

static void send_cmd(uint32_t command, uint32_t offset, uint16_t nsectors) {
    long int r = random();

    memset(cmd, 0 , 31);
    memcpy(cmd, "USBC", 4);

    if (r)          SETBE32(cmd+4, r);
    if (offset)     SETBE32(cmd+17, offset);
    if (nsectors)   SETBE16(cmd+22, nsectors);
    if (command)    SETBE32(cmd+12, command);

    libusb_bulk_transfer(h, ep|LIBUSB_ENDPOINT_OUT, cmd, sizeof(cmd), &tmp, 0);
}

static void send_buf(unsigned int s) {
    libusb_bulk_transfer(h, ep|LIBUSB_ENDPOINT_OUT, buf, s, &tmp, 0);
}

static void recv_res(void) {
    libusb_bulk_transfer(h, 1|LIBUSB_ENDPOINT_IN, res, sizeof(res), &tmp, 0);
}

static void recv_buf(unsigned int s) {
    libusb_bulk_transfer(h, 1|LIBUSB_ENDPOINT_IN, buf, s, &tmp, 0);
}

#define NEXT do { argc--;argv++; } while(0)

int main(int argc, char **argv) {
    struct libusb_device_descriptor dev_desc;
    struct libusb_config_descriptor *conf_desc;
    const struct t_pid *ppid = pidtab;
    ssize_t nr;
    uint32_t offset = 0, size = 0;
    uint16_t crc16;
    uint8_t flag = 0;
    char action;
    char *partname = NULL;

    info("rkflashtool v%d.%d\n", RKFLASHTOOL_VERSION_MAJOR,
                                 RKFLASHTOOL_VERSION_MINOR);

    NEXT; if (!argc) usage();

    action = **argv; NEXT;

    switch(action) {
    case 'b':
        if (argc > 1) usage();
        else if (argc == 1)
            flag = strtoul(argv[0], NULL, 0);
        break;
    case 'l':
    case 'L':
        if (argc) usage();
        break;
    case 'e':
    case 'r':
    case 'w':
        if (argc < 1 || argc > 2) usage();
        if (argc == 1) {
            partname = argv[0];
        } else {
            offset = strtoul(argv[0], NULL, 0);
            size   = strtoul(argv[1], NULL, 0);
        }
        break;
    case 'm':
    case 'M':
    case 'B':
    case 'i':
    case 'j':
        if (argc != 2) usage();
        offset = strtoul(argv[0], NULL, 0);
        size   = strtoul(argv[1], NULL, 0);
        break;
    case 'n':
    case 'v':
    case 'p':
    case 'P':
        if (argc) usage();
        offset = 0;
        size   = 1024;
        break;
    default:
        usage();
    }

    /* Initialize libusb */

    if (libusb_init(&c)) fatal("cannot init libusb\n");

    libusb_set_debug(c, 3);

    /* Detect connected RockChip device */

    while ( !h && ppid->pid) {
        h = libusb_open_device_with_vid_pid(c, 0x2207, ppid->pid);
        if (h) {
            info("Detected %s...\n", ppid->name);
            break;
        }
        ppid++;
    }
    if (!h) fatal("cannot open device\n");

    /* Connect to device */

    if (libusb_kernel_driver_active(h, 0) == 1) {
        info("kernel driver active\n");
        if (!libusb_detach_kernel_driver(h, 0))
            info("driver detached\n");
    }

    if (libusb_claim_interface(h, 0) < 0)
        fatal("cannot claim interface\n");
    info("interface claimed\n");

    if (libusb_get_device_descriptor(libusb_get_device(h), &dev_desc) != 0)
        fatal("cannot get device descriptor\n");

    if (dev_desc.iManufacturer == 0)
        info("MASKROM mode\n");

    if (libusb_get_active_config_descriptor(libusb_get_device(h), &conf_desc) != 0)
        fatal("cannot get config descriptor\n");

    ep = conf_desc->interface[0].altsetting[0].endpoint[1].bEndpointAddress;
    info("Using endpoint %u for bulk transfer\n", ep);

    switch(action) {
    case 'l':
        info("load DDR init\n");
        crc16 = 0xffff;
        while ((nr = read(0, buf, 4096)) == 4096) {
            crc16 = rkcrc16(crc16, buf, nr);
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1137, buf, nr, 0);
        }
        if (nr != -1) {
            crc16 = rkcrc16(crc16, buf, nr);
            buf[nr++] = crc16 >> 8;
            buf[nr++] = crc16 & 0xff;
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1137, buf, nr, 0);
        }
        goto exit;
    case 'L':
        info("load USB loader\n");
        crc16 = 0xffff;
        while ((nr = read(0, buf, 4096)) == 4096) {
            crc16 = rkcrc16(crc16, buf, nr);
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1138, buf, nr, 0);
        }
        if (nr != -1) {
            crc16 = rkcrc16(crc16, buf, nr);
            buf[nr++] = crc16 >> 8;
            buf[nr++] = crc16 & 0xff;
            libusb_control_transfer(h, LIBUSB_REQUEST_TYPE_VENDOR, 12, 0, 1138, buf, nr, 0);
        }
        goto exit;
    }

    /* Initialize bootloader interface */

    send_cmd(RKFT_CMD_TESTUNITREADY, 0, 0);
    recv_res();
    usleep(20*1000);

    /* Parse partition name */
    if (partname) {
        info("working with partition: %s\n", partname);

        /* Read parameters */
        offset = 0;
        send_cmd(RKFT_CMD_READLBA, offset, RKFT_OFF_INCR);
        recv_buf(RKFT_BLOCKSIZE);
        recv_res();

        /* Check parameter length */
        uint32_t *p = (uint32_t*)buf+1;
        size = *p;
        if (size > MAX_PARAM_LENGTH)
          fatal("Bad parameter length!\n");

        /* Search for mtdparts */
        const char *param = (const char *)&buf[8];
        const char *mtdparts = strstr(param, "mtdparts=");
        if (!mtdparts) {
            info("Error: 'mtdparts' not found in command line.\n");
            goto exit;
        }

        /* Search for '(partition_name)' */
        char partexp[256];
        snprintf(partexp, 256, "(%s)", partname);
        char *par = strstr(mtdparts, partexp);
        if (!par) {
            info("Error: Partition '%s' not found.\n", partname);
            goto exit;
        }

        /* Cut string by NULL-ing just before (partition_name) */
        par[0] = '\0';

        /* Search for '@' sign */
        char *arob = strrchr(mtdparts, '@');
        if (!arob) {
            info("Error: Bad syntax in mtdparts.\n");
            goto exit;
        }

        offset = strtoul(arob+1, NULL, 0);
        info("found offset: %#010x\n", offset);

        /* Cut string by NULL-ing just before '@' sign */
        arob[0] = '\0';

        /* Search for '-' sign (if last partition) */
        char *minus = strrchr(mtdparts, '-');
        if (minus) {

            /* Read size from NAND info */
            send_cmd(RKFT_CMD_READFLASHINFO, 0, 0);
            recv_buf(512);
            recv_res();

            nand_info *nand = (nand_info *) buf;
            size = nand->flash_size - offset;

            info("partition extends up to the end of NAND (size: 0x%08x).\n", size);
            goto action;
        }

        /* Search for ',' sign */
        char *comma = strrchr(mtdparts, ',');
        if (comma) {
            size = strtoul(comma+1, NULL, 0);
            info("found size: %#010x\n", size);
            goto action;
        }

        /* Search for ':' sign (if first partition) */
        char *colon = strrchr(mtdparts, ':');
        if (colon) {
            size = strtoul(colon+1, NULL, 0);
            info("found size: %#010x\n", size);
            goto action;
        }

        /* Error: size not found! */
        info("Error: Bad syntax for partition size.\n");
        goto exit;
    }

action:
    /* Check and execute command */

    switch(action) {
    case 'b':   /* Reboot device */
        info("rebooting device...\n");
        send_reset(flag);
        recv_res();
        break;
    case 'r':   /* Read FLASH */
        while (size > 0) {
            infocr("reading flash memory at offset 0x%08x", offset);

            send_cmd(RKFT_CMD_READLBA, offset, RKFT_OFF_INCR);
            recv_buf(RKFT_BLOCKSIZE);
            recv_res();

            if (write(1, buf, RKFT_BLOCKSIZE) <= 0)
                fatal("Write error! Disk full?\n");

            offset += RKFT_OFF_INCR;
            size   -= RKFT_OFF_INCR;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'w':   /* Write FLASH */
        while (size > 0) {
            infocr("writing flash memory at offset 0x%08x", offset);

            if (read(0, buf, RKFT_BLOCKSIZE) <= 0) {
                fprintf(stderr, "... Done!\n");
                info("premature end-of-file reached.\n");
                goto exit;
            }

            send_cmd(RKFT_CMD_WRITELBA, offset, RKFT_OFF_INCR);
            send_buf(RKFT_BLOCKSIZE);
            recv_res();

            offset += RKFT_OFF_INCR;
            size   -= RKFT_OFF_INCR;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'p':   /* Retrieve parameters */
        {
            uint32_t *p = (uint32_t*)buf+1;

            info("reading parameters at offset 0x%08x\n", offset);

            send_cmd(RKFT_CMD_READLBA, offset, RKFT_OFF_INCR);
            recv_buf(RKFT_BLOCKSIZE);
            recv_res();

            /* Check size */
            size = *p;
            info("size:  0x%08x\n", size);
            if (size > MAX_PARAM_LENGTH)
                fatal("Bad parameter length!\n");

            /* Check CRC */
            uint32_t crc_buf = *(uint32_t *)(buf + 8 + size),
                     crc = 0;
            crc = rkcrc32(crc, buf + 8, size);
            if (crc_buf != crc)
              fatal("bad CRC! (%#x, should be %#x)\n", crc_buf, crc);

            if (write(1, &buf[8], size) <= 0)
                fatal("Write error! Disk full?\n");
        }
        break;
    case 'P':   /* Write parameters */
        {
            /* Header */
            memcpy((char *)buf, "PARM", 4);

            /* Content */
            int sizeRead;
            if ((sizeRead = read(0, buf + 8, RKFT_BLOCKSIZE - 8)) < 0) {
                info("read error: %s\n", strerror(errno));
                goto exit;
            }

            /* Length */
            *(uint32_t *)(buf + 4) = sizeRead;

            /* CRC */
            uint32_t crc = 0;
            crc = rkcrc32(crc, buf + 8, sizeRead);
            PUT32LE(buf + 8 + sizeRead, crc);

            /*
             * The parameter file is written at 8 different offsets:
             * 0x0000, 0x0400, 0x0800, 0x0C00, 0x1000, 0x1400, 0x1800, 0x1C00
             */

            for(offset = 0; offset < 0x2000; offset += 0x400) {
                infocr("writing flash memory at offset 0x%08x", offset);
                send_cmd(RKFT_CMD_WRITELBA, offset, RKFT_OFF_INCR);
                send_buf(RKFT_BLOCKSIZE);
                recv_res();
            }
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'm':   /* Read RAM */
        while (size > 0) {
            int sizeRead = size > RKFT_BLOCKSIZE ? RKFT_BLOCKSIZE : size;
            infocr("reading memory at offset 0x%08x size %x", offset, sizeRead);

            send_cmd(RKFT_CMD_READSDRAM, offset, sizeRead);
            recv_buf(sizeRead);
            recv_res();

            if (write(1, buf, sizeRead) <= 0)
                fatal("Write error! Disk full?\n");

            offset += sizeRead;
            size -= sizeRead;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'M':   /* Write RAM */
        while (size > 0) {
            int sizeRead;
            if ((sizeRead = read(0, buf, RKFT_BLOCKSIZE)) <= 0) {
                info("premature end-of-file reached.\n");
                goto exit;
            }
            infocr("writing memory at offset 0x%08x size %x", offset, sizeRead);

            send_cmd(RKFT_CMD_WRITESDRAM, offset, sizeRead);
            send_buf(sizeRead);
            recv_res();

            offset += sizeRead;
            size -= sizeRead;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'B':   /* Exec RAM */
        info("booting kernel...\n");
        send_exec(offset, size);
        recv_res();
        break;
    case 'i':   /* Read IDB */
        while (size > 0) {
            int sizeRead = size > RKFT_IDB_INCR ? RKFT_IDB_INCR : size;
            infocr("reading IDB flash memory at offset 0x%08x", offset);

            send_cmd(RKFT_CMD_READSECTOR, offset, sizeRead);
            recv_buf(RKFT_IDB_BLOCKSIZE * sizeRead);
            recv_res();

            if (write(1, buf, RKFT_IDB_BLOCKSIZE * sizeRead) <= 0)
                fatal("Write error! Disk full?\n");

            offset += sizeRead;
            size -= sizeRead;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'j':   /* write IDB */
        while (size > 0) {
            infocr("writing IDB flash memory at offset 0x%08x", offset);

            memset(ibuf, RKFT_IDB_BLOCKSIZE, 0xff);
            if (read(0, ibuf, RKFT_IDB_DATASIZE) <= 0) {
                fprintf(stderr, "... Done!\n");
                info("premature end-of-file reached.\n");
                goto exit;
            }

            send_cmd(RKFT_CMD_WRITESECTOR, offset, 1);
            libusb_bulk_transfer(h, ep|LIBUSB_ENDPOINT_OUT, ibuf, RKFT_IDB_BLOCKSIZE, &tmp, 0);
            recv_res();
            offset += 1;
            size -= 1;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'e':   /* Erase flash */
        memset(buf, 0xff, RKFT_BLOCKSIZE);
        while (size > 0) {
            infocr("erasing flash memory at offset 0x%08x", offset);

            send_cmd(RKFT_CMD_WRITELBA, offset, RKFT_OFF_INCR);
            send_buf(RKFT_BLOCKSIZE);
            recv_res();

            offset += RKFT_OFF_INCR;
            size   -= RKFT_OFF_INCR;
        }
        fprintf(stderr, "... Done!\n");
        break;
    case 'v':   /* Read Chip Version */
        send_cmd(RKFT_CMD_READCHIPINFO, 0, 0);
        recv_buf(16);
        recv_res();

        info("chip version: %c%c%c%c-%c%c%c%c.%c%c.%c%c-%c%c%c%c\n",
            buf[ 3], buf[ 2], buf[ 1], buf[ 0],
            buf[ 7], buf[ 6], buf[ 5], buf[ 4],
            buf[11], buf[10], buf[ 9], buf[ 8],
            buf[15], buf[14], buf[13], buf[12]);
        break;
    case 'n':   /* Read NAND Flash Info */
    {
        send_cmd(RKFT_CMD_READFLASHID, 0, 0);
        recv_buf(5);
        recv_res();

        info("Flash ID: %02x %02x %02x %02x %02x\n",
            buf[0], buf[1], buf[2], buf[3], buf[4]);

        send_cmd(RKFT_CMD_READFLASHINFO, 0, 0);
        recv_buf(512);
        recv_res();

        nand_info *nand = (nand_info *) buf;
        uint8_t id = nand->manufacturer_id,
                cs = nand->chip_select;

        info("Flash Info:\n"
             "\tManufacturer: %s (%d)\n"
             "\tFlash Size: %dMB\n"
             "\tBlock Size: %dKB\n"
             "\tPage Size: %dKB\n"
             "\tECC Bits: %d\n"
             "\tAccess Time: %d\n"
             "\tFlash CS:%s%s%s%s\n",

             /* Manufacturer */
             id < MAX_NAND_ID ? manufacturer[id] : "Unknown",
             id,

             nand->flash_size >> 11, /* Flash Size */
             nand->block_size >> 1,  /* Block Size */
             nand->page_size  >> 1,  /* Page Size */
             nand->ecc_bits,         /* ECC Bits */
             nand->access_time,      /* Access Time */

             /* Flash CS */
             cs & 1 ? " <0>" : "",
             cs & 2 ? " <1>" : "",
             cs & 4 ? " <2>" : "",
             cs & 8 ? " <3>" : "");
    }
    default:
        break;
    }

exit:
    /* Disconnect and close all interfaces */

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(c);
    return 0;
}
