TARGET = Boro
OBJS = src/main.o src/sys.o src/gfx.o src/font.o src/ui.o src/home.o src/flac.o src/flac_browse.o src/flac_pages.o src/flac_playlists.o \
       src/tags.o src/library.o src/art.o src/player.o src/stations.o src/playlists.o src/volume.o src/settings.o src/scrub.o src/pod.o src/deck.o src/stats.o src/visual.o src/mp4.o src/avc.o src/mpegavc.o src/vplayer.o src/vlib.o src/video.o src/borokernel.o src/fonts.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -std=gnu11 -I$(shell psp-config --psp-prefix)/include/freetype2
ifdef SCRIPT
CFLAGS += -DBORO_SCRIPT
endif
ASFLAGS = $(CFLAGS)
LIBDIR =
LDFLAGS =
LIBS = -lfreetype -ljpeg -lpng -lz -lbz2 -lpspgu -lpsprtc -lpsppower -lpspmp3 -lpspaudiocodec -lpspaudio -lpsputility -lpspkubridge -lm

BUILD_PRX = 1

EXTRA_TARGETS = EBOOT.PBP kernel/boro_kernel.prx
EXTRA_CLEAN = kernel/main.o kernel/mecore.o kernel/boro_kernel.elf
PSP_EBOOT_TITLE = Boro
PSP_EBOOT_ICON = res/xmb/ICON0.PNG
PSP_EBOOT_PIC1 = res/xmb/PIC1.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

src/fonts.o: $(wildcard res/fonts/*.ttf)

# The kernel helper ships next to the EBOOT (see src/volume.h).
kernel/boro_kernel.prx: kernel/main.c kernel/mecore.S kernel/exports.exp
	$(MAKE) -C kernel
