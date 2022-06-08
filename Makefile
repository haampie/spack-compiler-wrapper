.PHONY: all clean
-include Make.user

SPACK_CFLAGS = -std=gnu99 -fPIC -fvisibility=hidden
# SPACK_LDFLAGS = -Wl,--version-script=./spack-compiler-wrapper.version

ifeq ($(OS), Darwin)
  SHLIBEXT = dylib
else
  SHLIBEXT = so
endif

all: spack-compiler-wrapper.$(SHLIBEXT)

%.o: %.c
	$(CC) $(CFLAGS) $(SPACK_CFLAGS) -c $<

spack-compiler-wrapper.$(SHLIBEXT): spack-compiler-wrapper.o
	$(CC) $(LDFLAGS) $(SPACK_LDFLAGS) -shared -o $@ $< -ldl

clean:
	rm -f *.o *.so
