# Winamp 5.x is 32-bit, so the plugin must be too.
# Needs: sudo apt install gcc-mingw-w64-i686
CC = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
LDFLAGS = -shared -s -static-libgcc -Wl,--enable-stdcall-fixup
LIBS = -lwininet -lgdi32 -luser32 -lkernel32

gen_stereo.dll: gen_stereo.c gen_stereo_res.o resource.h
	$(CC) $(CFLAGS) -o $@ gen_stereo.c gen_stereo_res.o $(LDFLAGS) $(LIBS)

gen_stereo_res.o: gen_stereo.rc resource.h
	$(WINDRES) -O coff -o $@ gen_stereo.rc

clean:
	rm -f gen_stereo.dll gen_stereo_res.o

.PHONY: clean
