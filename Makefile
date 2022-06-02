.PHONY: all clean

-include Make.user

SPACK_CFLAGS = -fPIC -fvisibility=hidden
SPACK_LDFLAGS = -Wl,--version-script=./spack-compiler-wrapper.version

all: spack-compiler-wrapper.so

%.o: %.c
	$(CC) $(CFLAGS) $(SPACK_CFLAGS) -c $<

spack-compiler-wrapper.so: spack-compiler-wrapper.o
	$(CC) $(LDFLAGS) $(SPACK_LDFLAGS) -shared -o $@ $< -ldl

test: test.o spack-compiler-wrapper.o
	$(CC) $(LDFLAGS) $(SPACK_LDFLAGS) -o $@ $^ -ldl

clean:
	rm -f *.o *.so
