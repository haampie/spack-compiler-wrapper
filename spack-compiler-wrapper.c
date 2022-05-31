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
static const char * spack_cc[] = {
    "cc", "c89", "c99", "gcc", "clang",
    "armclang", "icc", "icx", "pgcc", "nvc",
    "xlc", "xlc_r", "fcc", "amdclang"
};

// SPACK_CXX
static const char * spack_cxx[] = {
    "c++", "CC", "g++", "clang++",
    "armclang++", "icpc", "icpx",
    "dpcpp", "pgc++", "nvc++", "xlc++",
    "xlc++_r", "FCC", "amdclang++"
};

// SPACK_FC
static const char * spack_fc[] = {
    "ftn", "f90", "fc", "f95", "gfortran",
    "flang", "armflang", "ifort", "ifx",
    "pgfortran", "nvfortran", "xlf90",
    "xlf90_r", "nagfor", "frt", "amdflang"
};

// SPACK_F77
static const char * spack_f77[] = {
    "f77", "xlf", "xlf_r", "pgf77", "amdflang"
};

// SPACK_LD
static const char * spack_ld[] = {
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

static const char * get_filename(const char *p) {
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

static const char * get_spack_variable(enum executable_t type) {
    switch (type) {
    case SPACK_CC:
        return "SPACK_CC";
    case SPACK_CXX:
        return "SPACK_CXX";
    case SPACK_FC:
        return "SPACK_FC";
    case SPACK_F77:
        return "SPACK_F77";
    case SPACK_LD:
        return "SPACK_LD";
    }
    return NULL;
}

static const char * override_path(enum executable_t type) {
    char const *var = get_spack_variable(type);
    char const *path = getenv(var);
    if (path) return path;
    fprintf(stderr, "%s not set\n", var);
    exit(1);
}

// Compiler wrapper stuff

static int system_path(const char * p) {
    char * sysdir = getenv("SPACK_SYSTEM_DIRS");
    if (sysdir == NULL) return 0;
    char * end;
    while (1) {
        end = strchr(sysdir, ':');
        size_t len = end == NULL ? strlen(sysdir) : end - sysdir;
        // Todo, remove trailing / from end?
        if (strncmp(p, sysdir, len) == 0 && (p[len] == '\0' || p[len] == '/')) return 1;
        if (end == NULL) break;
        sysdir = end + 1;
    }
    return 0;
}

static void compiler_wrapper(char *const * argv, enum executable_t type) {
    for (size_t j = 0; argv[j] != NULL; ++j) {
        char *arg = argv[j];

        // Skip non-flags
        if (*arg != '-' || arg[1] == '\0') continue;
        ++arg;

        if (type == SPACK_LD) {
            // Linking fix up: --enable-new-dtags, --disable-new-dtags, -L,
            // -rpath <path>, --rpath <path>, -rpath=<path>, --rpath=<path>.
            if (*arg == 'L') {
                ++arg; if (*arg == '\0' && (arg = argv[++j]) == NULL) break;
                printf("[ld] Linker search path: %s (system path: %s)\n", arg, system_path(arg) ? "yes" : "no");
            } else if (*arg == 'l') {
                ++arg; if (*arg == '\0' && (arg = argv[++j]) == NULL) break;
                printf("[ld] Library: %s\n", arg);
            } else if (strcmp(arg, "-enable-new-dtags") == 0) {
                printf("[ld] Using --enable-new-dtags\n");
            } else if (strcmp(arg, "-disable-new-dtags") == 0) {
                printf("[ld] Using --disable-new-dtags\n");
            } else {
                if (strncmp(arg, "rpath", 5) == 0) {
                   arg += 6;
                } else if (strncmp(arg, "-rpath", 6) == 0) {
                   arg += 7;
                } else {
                    continue;
                }

                if (*arg == '=') {
                    ++arg;
                } else if (*arg == '\0') {
                    // Ignore missing arguments
                    if ((arg = argv[++j]) == NULL) break;
                }

                printf("[ld] Set rpath: %s\n", arg);
            }
        } else {
            // Compilation fix up: -I, -isystem, etc.
            if (*arg == 'I') {
                ++arg; if (*arg == '\0' && (arg = argv[++j]) == NULL) break;
                printf("[cc] Include path: %s (system path: %s)\n", arg, system_path(arg) ? "yes" : "no");
            } else if (strcmp(arg, "system") == 0) {
                ++arg; if (*arg == '\0' && (arg = argv[++j]) == NULL) break;
                printf("[cc] System include path: %s (system path: %s)\n", arg, system_path(arg) ? "yes" : "no");
            }
        }
    }
}

// The exec* + posix_spawn calls we wrap

__attribute__ ((visibility ("default"))) 
int execve(const char *path, char *const *argv, char *const *envp) {
    printf("Intercepting %s\n", path);
    //for (size_t i = 0; envp[i] != NULL; ++i) {
    //    printf("%s\n", envp[i]);
    //}
    enum executable_t type = compiler_type(get_filename(path));
    if (type != SPACK_NONE) {
        path = override_path(type);
        compiler_wrapper(argv, type);
    }
    typeof(execve) *real = dlsym(RTLD_NEXT, "execve");
    return real(path, argv, envp);
}


__attribute__ ((visibility ("default"))) 
int execvpe(const char *file, char *const *argv, char *const *envp) {
    printf("Intercepting %s\n", file);
    enum executable_t type = compiler_type(get_filename(file));
    if (type != SPACK_NONE) {
        file = override_path(type);
        compiler_wrapper(argv, type);
    }
    
    for (int i = 0; envp[i]; i++)
        putenv(envp[i]);
    
    typeof(execvpe) *real = dlsym(RTLD_NEXT, "execvp");
    return real(file, argv, environ);
}

__attribute__ ((visibility ("default")))
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const *argv, char *const *envp) {
    printf("Intercepting %s\n", path);
    enum executable_t type = compiler_type(get_filename(path));
    if (type != SPACK_NONE) {
        path = override_path(type);
        compiler_wrapper(argv, type);
    }
    typeof(posix_spawn) *real = dlsym(RTLD_NEXT, "posix_spawn");
    return real(pid, path, file_actions, attrp, argv, envp);
}

// Fallback to execve / execvpe

__attribute__ ((visibility ("default"))) 
int execl(const char *path, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
    copy_args(argv, arg0, &ap);
    va_end(ap);
    return execve(path, argv, environ);
}

__attribute__ ((visibility ("default"))) 
int execlp(const char *file, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
    copy_args(argv, arg0, &ap);
    va_end(ap);
    return execvpe(file, argv, environ);
}

__attribute__ ((visibility ("default"))) 
int execle(const char *path, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
    copy_args(argv, arg0, &ap);
    char **env = va_arg(ap, char **);
    va_end(ap);
    return execve(path, argv, env);
}

__attribute__ ((visibility ("default"))) 
int execv(const char *path, char *const *argv) {
    return execve(path, argv, environ);
}

__attribute__ ((visibility ("default"))) 
int execvp(const char *file, char *const *argv) {
    return execvpe(file, argv, environ);
}

