.PHONY: all clean install

SPACK_CFLAGS = -std=gnu99 -fPIC -fvisibility=hidden
SPACK_LDFLAGS = -Wl,--version-script=./spack-compiler-wrapper.version

prefix = /usr/local
exec_prefix = $(prefix)
libexecdir = $(exec_prefix)/libexec

all: spack-compiler-wrapper.so

%.o: %.c
	$(CC) $(CFLAGS) $(SPACK_CFLAGS) -c $<

spack-compiler-wrapper.so: spack-compiler-wrapper.o
	$(CC) $(LDFLAGS) $(SPACK_LDFLAGS) -shared -o $@ $< -ldl

install: all
	mkdir -p $(DESTDIR)$(libexecdir)
	cp -p spack-compiler-wrapper.so $(DESTDIR)$(libexecdir)

clean:
	rm -f spack-compiler-wrapper.o spack-compiler-wrapper.so

-include Make.user
