TARGET = Boro
OBJS = src/main.o src/sys.o src/gfx.o src/font.o src/ui.o src/home.o src/flac.o \
       src/tags.o src/library.o src/art.o src/player.o src/fonts.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu11 -I$(shell psp-config --psp-prefix)/include/freetype2
ifdef SCRIPT
CFLAGS += -DBORO_SCRIPT
endif
ASFLAGS = $(CFLAGS)
LIBDIR =
LDFLAGS =
LIBS = -lfreetype -ljpeg -lpng -lz -lbz2 -lpspgu -lpsprtc -lpsppower -lpspmp3 -lpspaudio -lpsputility -lm

BUILD_PRX = 1

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Boro

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

src/fonts.o: $(wildcard res/fonts/*.ttf)
