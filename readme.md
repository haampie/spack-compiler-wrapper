Spack compiler wrapper through `LD_PRELOAD`.

Inspired by mold's `mold -run ...` to intercept `ld` calls and use `mold` instead.

The idea is:
1. Make `spack-compiler-wrapper` a build dependency of any package
2. Build `spack-compiler-wrapper` itself without a compiler wrapper (it's just a shared lib depending on libc)
3. Make `spack-compiler-wrapper` set `LD_PRELOAD=<prefi>/spack-compiler-wrapper.so`
4. `spack-compiler-wrapper.so` defines `exec*` symbols, which intercepts known compiler/linker calls, and execute `SPACK_*` compilers/linkers instead.

Example:

```console
$ make
$ echo 'int main(){return 0;}' > example.c
$ echo '#!/bin/sh' > build.sh
$ echo '/usr/bin/gcc -v -Wl,-v example.c' >> build.sh
$ chmod +x build.sh

# Builds with gcc and ld as expected
$ ./build.sh

# Builds with clang and lld instead
$ export SPACK_CC=/usr/bin/clang
$ export SPACK_LD=/usr/bin/ld.lld
$ export LD_PRELOAD=$PWD/spack-compiler-wrapper.so
$ ./build.sh
```

- [X] `SPACK_*FLAGS` where `* = C/CXX/F`;
- [X] `SPACK_LDFLAGS` (1. injected as compiler flag, not linker flag; 2. correct for multiple `-c`, `-E`, `-S` flags);
- [X] `SPACK_TARGET_ARGS`
- [X] `SPACK_RPATH_DIRS`, `SPACK_COMPILER_EXTRA_RPATHS`, `SPACK_COMPILER_IMPLICIT_RPATHS`
- [X] `SPACK_INCLUDE_DIRS`
- [X] `SPACK_LINK_DIRS`
- [X] `SPACK_TEST_COMMAND=dump-args`
- [ ] `SPACK_CPPFLAGS`
- [ ] `SPACK_CCACHE_BINARY`
- [ ] `SPACK_TEST_COMMAND=dump-env`
- [ ] `SPACK_DEBUG=TRUE`
