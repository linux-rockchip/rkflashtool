# Simple Makefile for RK Flash Tool

CC	= $(CROSSPREFIX)gcc
LD	= $(CC)
CFLAGS	= -O2 -Wall -Wextra
LDFLAGS	=
PREFIX ?= usr/local

PKGCONFIG ?= $(shell pkg-config --exists libusb-1.0 && echo 1)

ifeq ($(PKGCONFIG),1)
CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDFLAGS += $(shell pkg-config --libs libusb-1.0)
else ifdef LIBUSB
CFLAGS	+= -I$(LIBUSB)/include
LDFLAGS	+= -L$(LIBUSB)/lib
else
CFLAGS	+= -I/usr/include/libusb-1.0
LDFLAGS += -lusb-1.0
endif


MACH	= $(shell $(CC) -dumpmachine)
ifeq ($(findstring mingw,$(MACH)),mingw)
LDFLAGS	+= -s -static -lmman
USE_RES	= 1
endif
ifeq ($(findstring cygwin,$(MACH)),cygwin)
LDFLAGS	+= -s
USE_RES	= 1
endif

ifeq ($(USE_RES),1)
RC	= $(CROSSPREFIX)windres
RCFLAGS	= -O coff -i
BINEXT	= .exe
RESFILE	= %.res
AWK	= awk
VERMAJ	= $(shell $(AWK) '/define.*RKFLASHTOOL_VERSION_MAJOR/{print $$3}' version.h)
VERMIN	= $(shell $(AWK) '/define.*RKFLASHTOOL_VERSION_MINOR/{print $$3}' version.h)
VERREV	= 0
LCOPYR	= 2010-2013 Ivo van Poorten, Fukaumi Naoki, Guenter Knauf, Ulrich Prinz, Steve Wilson
FDESCR	= Flashtool for RK2808, RK2818, RK2918, RK3066, RK3068 and RK3188 based tablets
WWWURL	= http://sourceforge.net/projects/rkflashtool/
ifeq ($(findstring /sh,$(SHELL)),/sh)
DL	= '
endif
endif

PROGS	= $(patsubst %.c,%$(BINEXT), $(wildcard *.c))
SCRIPTS = rkunsign rkparametersblock rkmisc rkpad rkparameters

all: $(PROGS) $(SCRIPTS)

%$(BINEXT): %.c $(RESFILE)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

install: $(PROGS) $(SCRIPTS)
	install -d -m 0755 $(DESTDIR)/$(PREFIX)/bin
	install -m 0755 $(PROGS) $(DESTDIR)/$(PREFIX)/bin
	install -m 0755 $(SCRIPTS) $(DESTDIR)/$(PREFIX)/bin

clean:
	$(RM) $(PROGS) *.res *.rc *.zip *.tar.gz *.tar.bz2 *.tar.xz *~ *.exe

uninstall:
	cd $(DESTDIR)/$(PREFIX)/bin && $(RM) -f $(PROGS) $(SCRIPTS)

%.res: %.rc
	$(RC) $(RCFLAGS) $< -o $@

%.rc: Makefile
	@echo $(DL)1 VERSIONINFO $(DL)>$@
	@echo $(DL) FILEVERSION $(VERMAJ),$(VERMIN),$(VERREV),0 $(DL)>>$@
	@echo $(DL) PRODUCTVERSION $(VERMAJ),$(VERMIN),$(VERREV),0 $(DL)>>$@
	@echo $(DL) FILEFLAGSMASK 0x3fL $(DL)>>$@
	@echo $(DL) FILEOS 0x40004L $(DL)>>$@
	@echo $(DL) FILEFLAGS 0x0L $(DL)>>$@
	@echo $(DL) FILETYPE 0x1L $(DL)>>$@
	@echo $(DL) FILESUBTYPE 0x0L $(DL)>>$@
	@echo $(DL)BEGIN $(DL)>>$@
	@echo $(DL)  BLOCK "StringFileInfo" $(DL)>>$@
	@echo $(DL)  BEGIN $(DL)>>$@
	@echo $(DL)    BLOCK "040904E4" $(DL)>>$@
	@echo $(DL)    BEGIN $(DL)>>$@
	@echo $(DL)      VALUE "LegalCopyright","\251 $(LCOPYR)\\0" $(DL)>>$@
#	@echo $(DL)      VALUE "CompanyName","$(COMPANY)\\0" $(DL)>>$@
	@echo $(DL)      VALUE "ProductName","$(notdir $(@:.rc=)).exe\\0" $(DL)>>$@
	@echo $(DL)      VALUE "ProductVersion","$(VERMAJ).$(VERMIN).$(VERREV)\\0" $(DL)>>$@
	@echo $(DL)      VALUE "License","Released under BSD license.\\0" $(DL)>>$@
	@echo $(DL)      VALUE "FileDescription","$(FDESCR)\\0" $(DL)>>$@
	@echo $(DL)      VALUE "FileVersion","$(VERMAJ).$(VERMIN).$(VERREV)\\0" $(DL)>>$@
	@echo $(DL)      VALUE "InternalName","$(notdir $(@:.rc=))\\0" $(DL)>>$@
	@echo $(DL)      VALUE "OriginalFilename","$(notdir $(@:.rc=)).exe\\0" $(DL)>>$@
	@echo $(DL)      VALUE "WWW","$(WWWURL)\\0" $(DL)>>$@
	@echo $(DL)    END $(DL)>>$@
	@echo $(DL)  END $(DL)>>$@
	@echo $(DL)  BLOCK "VarFileInfo" $(DL)>>$@
	@echo $(DL)  BEGIN $(DL)>>$@
	@echo $(DL)    VALUE "Translation", 0x409, 1252 $(DL)>>$@
	@echo $(DL)  END $(DL)>>$@
	@echo $(DL)END $(DL)>>$@

