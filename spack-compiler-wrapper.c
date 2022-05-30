#define _GNU_SOURCE 1

#include <alloca.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum executable_t {
    SPACK_CC,
    SPACK_CXX,
    SPACK_FC,
    SPACK_F77,
    SPACK_LD,
    SPACK_NONE
};

// SPACK_CC
const char * spack_cc[] = {
    "cc", "c89", "c99", "gcc", "clang",
    "armclang", "icc", "icx", "pgcc", "nvc",
    "xlc", "xlc_r", "fcc", "amdclang"
};

// SPACK_CXX
const char * spack_cxx[] = {
    "c++", "CC", "g++", "clang++",
    "armclang++", "icpc", "icpx",
    "dpcpp", "pgc++", "nvc++", "xlc++",
    "xlc++_r", "FCC", "amdclang++"
};

// SPACK_FC
const char * spack_fc[] = {
    "ftn", "f90", "fc", "f95", "gfortran",
    "flang", "armflang", "ifort", "ifx",
    "pgfortran", "nvfortran", "xlf90",
    "xlf90_r", "nagfor", "frt", "amdflang"
};

// SPACK_F77
const char * spack_f77[] = {
    "f77", "xlf", "xlf_r", "pgf77", "amdflang"
};

// SPACK_LD
const char * spack_ld[] = {
    "ld", "ld.gold", "ld.lld", "ld.bfd", "ld.mold"
};

extern char **environ;

static int count_args(va_list *ap) {
  va_list aq;
  va_copy(aq, *ap);

  int i = 0;
  while (va_arg(aq, char *))
    i++;
  va_end(aq);
  return i;
}

static void copy_args(char **argv, const char *arg0, va_list *ap) {
  int i = 1;
  char *arg;
  while ((arg = va_arg(*ap, char *)))
    argv[i++] = arg;

  ((const char **)argv)[0] = arg0;
  ((const char **)argv)[i] = NULL;
}

const char * get_filename(const char *p) {
    char * f = strrchr(p, '/');
    if (f == NULL) return p;
    return f + 1;
}

static enum executable_t compiler_type(const char *filename) {
    for (size_t j = 0; j < sizeof(spack_cc) / sizeof(char *); ++j)
        if (strcmp(filename, spack_cc[j]) == 0)
            return SPACK_CC;
    for (size_t j = 0; j < sizeof(spack_cxx) / sizeof(char *); ++j)
        if (strcmp(filename, spack_cxx[j]) == 0)
            return SPACK_CXX;
    for (size_t j = 0; j < sizeof(spack_fc) / sizeof(char *); ++j)
        if (strcmp(filename, spack_fc[j]) == 0)
            return SPACK_FC;
    for (size_t j = 0; j < sizeof(spack_f77) / sizeof(char *); ++j)
        if (strcmp(filename, spack_f77[j]) == 0)
            return SPACK_F77;
    for (size_t j = 0; j < sizeof(spack_ld) / sizeof(char *); ++j)
        if (strcmp(filename, spack_ld[j]) == 0)
            return SPACK_LD;
    return SPACK_NONE;
}

const char * override_path(enum executable_t type) {
    char *path;
    char *var;
    switch (type) {
    case SPACK_CC:
        var ="SPACK_CC";
        break;
    case SPACK_CXX:
        var ="SPACK_CXX";
        break;
    case SPACK_FC:
        var ="SPACK_FC";
        break;
    case SPACK_F77:
        var ="SPACK_F77";
        break;
    case SPACK_LD:
        var ="SPACK_LD";
        break;
    }
    path = getenv(var);
    if (path) return path;
    fprintf(stderr, "%s is not set\n", var);
    exit(1);
}

int execvpe(const char *file, char *const *argv, char *const *envp) {
  enum executable_t type = compiler_type(get_filename(file));
  if (type != SPACK_NONE)
    file = override_path(type);

  for (int i = 0; envp[i]; i++)
    putenv(envp[i]);

  typeof(execvpe) *real = dlsym(RTLD_NEXT, "execvp");
  return real(file, argv, environ);
}

int execve(const char *path, char *const *argv, char *const *envp) {
  enum executable_t type = compiler_type(get_filename(path));
  if (type != SPACK_NONE)
    path = override_path(type);
  typeof(execve) *real = dlsym(RTLD_NEXT, "execve");
  return real(path, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const *argv, char *const *envp) {
  enum executable_t type = compiler_type(get_filename(path));
  if (type != SPACK_NONE)
    path = override_path(type);
  typeof(posix_spawn) *real = dlsym(RTLD_NEXT, "posix_spawn");
  return real(pid, path, file_actions, attrp, argv, envp);
}

//

int execl(const char *path, const char *arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
  copy_args(argv, arg0, &ap);
  va_end(ap);
  return execve(path, argv, environ);
}

int execlp(const char *file, const char *arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
  copy_args(argv, arg0, &ap);
  va_end(ap);
  return execvpe(file, argv, environ);
}

int execle(const char *path, const char *arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
  copy_args(argv, arg0, &ap);
  char **env = va_arg(ap, char **);
  va_end(ap);
  return execve(path, argv, env);
}

int execv(const char *path, char *const *argv) {
  return execve(path, argv, environ);
}

int execvp(const char *file, char *const *argv) {
  return execvpe(file, argv, environ);
}

