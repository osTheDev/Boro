TARGET = Boro
OBJS = src/main.o src/sys.o src/gfx.o src/font.o src/home.o src/fonts.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu11 -I$(shell psp-config --psp-prefix)/include/freetype2
ASFLAGS = $(CFLAGS)
LIBDIR =
LDFLAGS =
LIBS = -lfreetype -lpng -lz -lbz2 -lpspgu -lpsprtc -lpsppower -lm

BUILD_PRX = 1

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Boro

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

src/fonts.o: res/fonts/Sora-400.ttf res/fonts/Sora-700.ttf res/fonts/Sora-800.ttf
