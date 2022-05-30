.PHONY: all clean

-include Make.user

CFLAGS ?= -fPIC

all: spack-compiler-wrapper.so

spack-compiler-wrapper.o: spack-compiler-wrapper.c
	$(CC) $(CFLAGS) -c $<

spack-compiler-wrapper.so: spack-compiler-wrapper.o
	$(CC) $(LDFLAGS) -shared -o $@ $< -ldl

clean:
	rm -f *.o *.so
