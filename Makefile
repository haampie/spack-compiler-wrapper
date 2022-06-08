.PHONY: all clean
-include Make.user

SPACK_CFLAGS = -std=gnu99 -fPIC -fvisibility=hidden
SPACK_LDFLAGS = -Wl,--version-script=./spack-compiler-wrapper.version

all: spack-compiler-wrapper.so

%.o: %.c
	$(CC) $(CFLAGS) $(SPACK_CFLAGS) -c $<

spack-compiler-wrapper.so: spack-compiler-wrapper.o
	$(CC) $(LDFLAGS) $(SPACK_LDFLAGS) -shared -o $@ $< -ldl

clean:
	rm -f spack-compiler-wrapper.o spack-compiler-wrapper.so
