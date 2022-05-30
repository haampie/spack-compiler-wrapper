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
$ echo 'gcc -v -Wl,-v example.c' > build.sh
$ chmod +x build.sh
$ ./build.sh # builds with gcc and ld as expected
$ SPACK_CC=/usr/bin/clang SPACK_LD=/usr/bin/ld.lld LD_PRELOAD=$PWD/spack-compiler-wrapper.so ./build.sh # builds with clang and lld instead.
```
