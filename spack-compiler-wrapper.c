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

enum mode_t {
  SPACK_MODE_CCLD, // preprocess, compile, assemble, link
  SPACK_MODE_CC,   // preprocess, compile, assemble (-c)
  SPACK_MODE_AS,   // preprocess, compile (-S)
  SPACK_MODE_CPP,  // preprocess (-E)
};

extern char **environ;

// SPACK_CC
static const char *spack_cc[] = {"cc",       "c89",   "c99", "gcc",     "clang",
                                 "armclang", "icc",   "icx", "pgcc",    "nvc",
                                 "xlc",      "xlc_r", "fcc", "amdclang"};

// SPACK_CXX
static const char *spack_cxx[] = {
    "c++",   "CC",    "g++",   "clang++", "armclang++", "icpc", "icpx",
    "dpcpp", "pgc++", "nvc++", "xlc++",   "xlc++_r",    "FCC",  "amdclang++"};

// SPACK_FC
static const char *spack_fc[] = {
    "ftn",      "f90",    "fc",  "f95",       "gfortran",  "flang",
    "armflang", "ifort",  "ifx", "pgfortran", "nvfortran", "xlf90",
    "xlf90_r",  "nagfor", "frt", "amdflang"};

// SPACK_F77
static const char *spack_f77[] = {"f77", "xlf", "xlf_r", "pgf77", "amdflang"};

// SPACK_LD
static const char *spack_ld[] = {"ld", "ld.gold", "ld.lld", "ld.bfd",
                                 "ld.mold"};

struct string_table_t {
  char *arr;
  size_t n;
  size_t capacity;
};

struct offset_list_t {
  size_t *offsets;
  size_t n;
  size_t capacity;
};

void string_table_init(struct string_table_t *t) {
  t->arr = NULL;
  t->n = 0;
  t->capacity = 0;
}

void string_table_reserve(struct string_table_t *t, size_t n) {
  if (t->n + n <= t->capacity)
    return;
  t->capacity = 2 * (t->n + n);
  char *arr = realloc(t->arr, t->capacity * sizeof(char));
  if (arr == NULL)
    exit(1);
  t->arr = arr;
}

size_t string_table_store(struct string_table_t *t, char const *str) {
  size_t n = strlen(str) + 1;
  string_table_reserve(t, n);
  size_t offset = t->n;
  memcpy(t->arr + offset, str, n);
  t->n += n;
  return offset;
}

size_t string_table_store_n(struct string_table_t *t, char const *str,
                            size_t n) {
  string_table_reserve(t, n);
  size_t offset = t->n;
  memcpy(t->arr + offset, str, n);
  t->n += n;
  return offset;
}

size_t string_table_store_flag(struct string_table_t *t, char const *flag,
                               char const *value) {
  size_t flag_len = strlen(flag);
  size_t val_len = strlen(value);
  size_t n = flag_len + val_len + 1;
  string_table_reserve(t, n);
  size_t offset = t->n;
  memcpy(t->arr + offset, flag, flag_len);
  memcpy(t->arr + offset + flag_len, value, val_len + 1);
  t->n += n;
  return offset;
}

size_t string_table_store_flag_n(struct string_table_t *t, char const *flag,
                                 char const *value, size_t val_len) {
  size_t flag_len = strlen(flag);
  size_t n = flag_len + val_len;
  string_table_reserve(t, n);
  size_t offset = t->n;
  memcpy(t->arr + offset, flag, flag_len);
  memcpy(t->arr + offset + flag_len, value, val_len);
  t->n += n;
  return offset;
}

void offset_list_init(struct offset_list_t *t) {
  t->offsets = NULL;
  t->n = 0;
  t->capacity = 0;
}

void offset_list_reserve(struct offset_list_t *t) {
  if (t->n < t->capacity)
    return;
  t->capacity = 2 * (t->n + 1);
  size_t *arr = realloc(t->offsets, t->capacity * sizeof(size_t));
  if (arr == NULL)
    exit(1);
  t->offsets = arr;
}

void offset_list_push(struct offset_list_t *t, size_t offset) {
  offset_list_reserve(t);
  t->offsets[t->n++] = offset;
}

struct state_t {
  enum mode_t mode;
  struct string_table_t strings;

  // -march etc
  struct offset_list_t spack_compiler_flags;

  // -I
  struct offset_list_t isystem_include_flags;
  struct offset_list_t include_flags;
  struct offset_list_t spack_include_flags;
  struct offset_list_t isystem_system_include_flags;
  struct offset_list_t system_include_flags;

  // -L
  struct offset_list_t lib_flags;
  struct offset_list_t spack_lib_flags;
  struct offset_list_t system_lib_flags;

  // -rpath=
  struct offset_list_t rpath_flags;
  struct offset_list_t spack_rpath_flags;
  struct offset_list_t system_rpath_flags;

  struct offset_list_t other_flags;
};

static void arg_parse_init(struct state_t *s) {
  s->mode = SPACK_MODE_CCLD;
  string_table_init(&s->strings);

  offset_list_init(&s->spack_compiler_flags);

  offset_list_init(&s->isystem_include_flags);
  offset_list_init(&s->include_flags);
  offset_list_init(&s->spack_include_flags);
  offset_list_init(&s->isystem_system_include_flags);
  offset_list_init(&s->system_include_flags);

  offset_list_init(&s->lib_flags);
  offset_list_init(&s->spack_lib_flags);
  offset_list_init(&s->system_lib_flags);

  offset_list_init(&s->rpath_flags);
  offset_list_init(&s->spack_rpath_flags);
  offset_list_init(&s->system_rpath_flags);

  offset_list_init(&s->other_flags);
}

// re-assemble the command line arguments
static char *const *arg_parse_finish(const char *argv0, struct state_t *s) {
  size_t n = s->spack_compiler_flags.n + s->isystem_include_flags.n +
             s->include_flags.n + s->spack_include_flags.n +
             s->isystem_system_include_flags.n + s->system_include_flags.n +
             s->lib_flags.n + s->spack_lib_flags.n + s->system_lib_flags.n +
             s->rpath_flags.n + s->spack_rpath_flags.n +
             s->system_rpath_flags.n + s->other_flags.n;
  // todo, use alloca
  char **argv = malloc((n + 2) * sizeof(char *));

  size_t exe_offset = string_table_store(&s->strings, argv0);

  argv[0] = s->strings.arr + exe_offset;

  size_t i = 1;

  // -march, cflags, etc
  for (size_t j = 0; j < s->spack_compiler_flags.n; ++j)
    argv[i++] = s->strings.arr + s->spack_compiler_flags.offsets[j];

  // -I
  for (size_t j = 0; j < s->isystem_include_flags.n; ++j)
    argv[i++] = s->strings.arr + s->isystem_include_flags.offsets[j];
  for (size_t j = 0; j < s->include_flags.n; ++j)
    argv[i++] = s->strings.arr + s->include_flags.offsets[j];
  for (size_t j = 0; j < s->spack_include_flags.n; ++j)
    argv[i++] = s->strings.arr + s->spack_include_flags.offsets[j];
  for (size_t j = 0; j < s->isystem_system_include_flags.n; ++j)
    argv[i++] = s->strings.arr + s->isystem_system_include_flags.offsets[j];
  for (size_t j = 0; j < s->system_include_flags.n; ++j)
    argv[i++] = s->strings.arr + s->system_include_flags.offsets[j];

  // -L
  for (size_t j = 0; j < s->lib_flags.n; ++j)
    argv[i++] = s->strings.arr + s->lib_flags.offsets[j];
  for (size_t j = 0; j < s->spack_lib_flags.n; ++j)
    argv[i++] = s->strings.arr + s->spack_lib_flags.offsets[j];
  for (size_t j = 0; j < s->system_lib_flags.n; ++j)
    argv[i++] = s->strings.arr + s->system_lib_flags.offsets[j];

  // -rpath=
  for (size_t j = 0; j < s->system_rpath_flags.n; ++j)
    argv[i++] = s->strings.arr + s->system_rpath_flags.offsets[j];
  for (size_t j = 0; j < s->spack_rpath_flags.n; ++j)
    argv[i++] = s->strings.arr + s->spack_rpath_flags.offsets[j];
  for (size_t j = 0; j < s->rpath_flags.n; ++j)
    argv[i++] = s->strings.arr + s->rpath_flags.offsets[j];

  // others
  for (size_t j = 0; j < s->other_flags.n; ++j)
    argv[i++] = s->strings.arr + s->other_flags.offsets[j];
  return argv;
}

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

static const char *get_filename(const char *p) {
  char *f = strrchr(p, '/');
  if (f == NULL)
    return p;
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

static const char *get_spack_variable(enum executable_t type) {
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

static const char *override_path(enum executable_t type) {
  char const *var = get_spack_variable(type);
  char const *path = getenv(var);
  if (path)
    return path;
  fprintf(stderr, "%s not set\n", var);
  exit(1);
}

// Compiler wrapper stuff

static int system_path(const char *p) {
  char *sysdir = getenv("SPACK_SYSTEM_DIRS");
  if (sysdir == NULL)
    return 0;
  char *end;
  while (1) {
    end = strchr(sysdir, ':');
    size_t len = end == NULL ? strlen(sysdir) : end - sysdir;
    // Todo, remove trailing / from end?
    if (strncmp(p, sysdir, len) == 0 && (p[len] == '\0' || p[len] == '/'))
      return 1;
    if (end == NULL)
      break;
    sysdir = end + 1;
  }
  return 0;
}

static void parse_compile_mode(char *const *argv, struct state_t *s) {
  for (size_t j = 0; argv[j] != NULL; ++j) {
    char *arg = argv[j];
    // Filter single character flags.
    if (arg[0] != '-' || arg[1] == '\0' || arg[2] != '\0')
      continue;
    enum mode_t mode = SPACK_MODE_CCLD;
    switch (arg[1]) {
    case 'c':
      mode = SPACK_MODE_CC;
      break;
    case 'S':
      mode = SPACK_MODE_AS;
      break;
    case 'E':
      mode = SPACK_MODE_CPP;
      break;
    }
    s->mode = s->mode > mode ? s->mode : mode;
  }
}

static void parse_ld(char *const *argv, struct state_t *s) {
  if (argv[0] == NULL)
    return;

  for (size_t j = 1; argv[j] != NULL; ++j) {
    char *arg = argv[j];

    // Skip non-flags
    if (*arg != '-' || arg[1] == '\0') {
      offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
      continue;
    }

    int is_rpath = 0;
    char *c = arg + 1;

    // Linking fix up: --enable-new-dtags, --disable-new-dtags, -L,
    // -rpath <path>, --rpath <path>, -rpath=<path>, --rpath=<path>.
    if (*c == 'L') {
      // Invalid input (value missing), but we'll pass it on.
      if (*++c == '\0' && (c = argv[++j]) == NULL) {
        offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
        break;
      }
      offset_list_push(system_path(c) ? &s->system_lib_flags : &s->lib_flags,
                       string_table_store_flag(&s->strings, "-L", c));
      continue;
    } else if (strcmp(c, "-enable-new-dtags") == 0 ||
               strcmp(c, "-disable-new-dtags") == 0) {
      // Drop the dtags flag, since we fix it.
      continue;
    } else if (strncmp(c, "rpath", 5) == 0) {
      is_rpath = 1;
      c += 5;
    } else if (strncmp(c, "-rpath", 6) == 0) {
      is_rpath = 1;
      c += 6;
    } else {
      // Flags we don't care about.
      offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
      continue;
    }

    if (is_rpath) {
      if (*c == '=') {
        ++c;
      } else if (*c == '\0' && (c = argv[++j]) == NULL) {
        offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
        break;
      }
      offset_list_push(system_path(c) ? &s->system_rpath_flags
                                      : &s->rpath_flags,
                       string_table_store_flag(&s->strings, "--rpath=", c));
    }
  }
}

static void parse_cc(char *const *argv, struct state_t *s) {
  if (argv[0] == NULL)
    return;

  for (size_t j = 1; argv[j] != NULL; ++j) {
    char *arg = argv[j];

    // Skip non-flags
    if (*arg != '-' || arg[1] == '\0') {
      offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
      continue;
    }

    char *c = arg + 1;
    // Compilation fix up: -I, -isystem, etc.
    if (*c == 'I') {
      if (*++c == '\0' && (c = argv[++j]) == NULL) {
        offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
        break;
      }
      offset_list_push(system_path(c) ? &s->system_include_flags
                                      : &s->include_flags,
                       string_table_store_flag(&s->strings, "-I", c));
    } else if (strncmp(c, "isystem", 7) == 0) {
      if (*(c += 7) == '\0' && (c = argv[++j]) == NULL) {
        offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
        break;
      }

      // Just split -system xxx for readability, even though
      // -isystem/path is allowed, apparently...
      if (system_path(c)) {
        offset_list_push(&s->isystem_system_include_flags,
                         string_table_store(&s->strings, "-isystem"));
        offset_list_push(&s->isystem_system_include_flags,
                         string_table_store(&s->strings, c));
      } else {
        offset_list_push(&s->isystem_include_flags,
                         string_table_store(&s->strings, "-isystem"));
        offset_list_push(&s->isystem_include_flags,
                         string_table_store(&s->strings, c));
      }
    } else {
      offset_list_push(&s->other_flags, string_table_store(&s->strings, arg));
    }
  }
}

static void parse_argv(char *const *argv, struct state_t *s,
                       enum executable_t type) {
  switch (type) {
  case SPACK_LD:
    parse_ld(argv, s);
    break;
  case SPACK_CC:
  case SPACK_CXX:
  case SPACK_FC:
  case SPACK_F77:
    parse_cc(argv, s);
    break;
  }
}

static void dump_args(struct state_t *s) {
  printf("isystem_system_include_flags: ");
  for (size_t i = 0; i < s->isystem_system_include_flags.n; ++i)
    printf("%s ", s->strings.arr + s->isystem_system_include_flags.offsets[i]);

  printf("\nisystem_include_flags: ");
  for (size_t i = 0; i < s->isystem_include_flags.n; ++i)
    printf("%s ", s->strings.arr + s->isystem_include_flags.offsets[i]);

  printf("\nsystem_include_flags: ");
  for (size_t i = 0; i < s->system_include_flags.n; ++i)
    printf("%s ", s->strings.arr + s->system_include_flags.offsets[i]);

  printf("\ninclude_flags: ");
  for (size_t i = 0; i < s->include_flags.n; ++i)
    printf("%s ", s->strings.arr + s->include_flags.offsets[i]);

  printf("\nsystem_lib_flags: ");
  for (size_t i = 0; i < s->system_lib_flags.n; ++i)
    printf("%s ", s->strings.arr + s->system_lib_flags.offsets[i]);

  printf("\nlib_flags: ");
  for (size_t i = 0; i < s->lib_flags.n; ++i)
    printf("%s ", s->strings.arr + s->lib_flags.offsets[i]);

  printf("\nsystem_rpath_flags: ");
  for (size_t i = 0; i < s->system_rpath_flags.n; ++i)
    printf("%s ", s->strings.arr + s->system_rpath_flags.offsets[i]);

  printf("\nrpath_flags: ");
  for (size_t i = 0; i < s->rpath_flags.n; ++i)
    printf("%s ", s->strings.arr + s->rpath_flags.offsets[i]);

  printf("\nother_flags: ");
  for (size_t i = 0; i < s->other_flags.n; ++i)
    printf("%s ", s->strings.arr + s->other_flags.offsets[i]);

  putchar('\n');
}

static void store_delimited_flags(char const *str, char delim, char const *flag,
                                  struct string_table_t *strings,
                                  struct offset_list_t *list) {
  if (str == NULL)
    return;
  char const *p = str;
  while (1) {
    char *end = strchr(p, delim);
    size_t len = end == NULL ? strlen(p) : end - p;
    // Copy the delimeter and replace it with \0.
    size_t start = strings->n;
    offset_list_push(list,
                     string_table_store_flag_n(strings, flag, p, len + 1));
    strings->arr[strings->n - 1] = '\0';
    if (end == NULL)
      return;
    p = end + 1;
  }
}

static void store_delimited(char const *str, char delim,
                            struct string_table_t *strings,
                            struct offset_list_t *list) {
  if (str == NULL)
    return;
  char const *p = str;
  while (1) {
    char *end = strchr(p, delim);
    size_t len = end == NULL ? strlen(p) : end - p;
    // Copy the delimeter, and replace with \0.
    size_t offset = string_table_store_n(strings, p, len + 1);
    strings->arr[offset + len] = '\0';
    offset_list_push(list, offset);
    if (end == NULL)
      return;
    p = end + 1;
  }
}

static void parse_spack_env(struct state_t *s, enum executable_t type) {
  switch (type) {
  case SPACK_LD:
    store_delimited_flags(getenv("SPACK_LINK_DIRS"), ':', "-L", &s->strings,
                          &s->spack_lib_flags);
    store_delimited_flags(getenv("SPACK_COMPILER_EXTRA_RPATHS"), ':', "-L",
                          &s->strings, &s->spack_lib_flags);
    store_delimited_flags(getenv("SPACK_RPATH_DIRS"), ':',
                          "-rpath=", &s->strings, &s->spack_rpath_flags);
    store_delimited_flags(getenv("SPACK_COMPILER_EXTRA_RPATHS"), ':',
                          "-rpath=", &s->strings, &s->spack_rpath_flags);
    store_delimited_flags(getenv("SPACK_COMPILER_IMPLICIT_RPATHS"), ':',
                          "-rpath=", &s->strings, &s->spack_rpath_flags);
    // TODO: store_delimited_flags(getenv("SPACK_LDLIBS"), ' ', "-l",
    // &s->strings, &s->other);
    break;
  case SPACK_CC:
    store_delimited(getenv("SPACK_CFLAGS"), ' ', &s->strings,
                    &s->spack_compiler_flags);
    break;
  case SPACK_CXX:
    store_delimited(getenv("SPACK_CXXFLAGS"), ' ', &s->strings,
                    &s->spack_compiler_flags);
    break;
  case SPACK_F77:
    store_delimited(getenv("SPACK_FFLAGS"), ' ', &s->strings,
                    &s->spack_compiler_flags);
    break;
  case SPACK_FC:
    store_delimited(getenv("SPACK_FFLAGS"), ' ', &s->strings,
                    &s->spack_compiler_flags);
    break;
  }
  switch (type) {
  case SPACK_CC:
  case SPACK_CXX:
  case SPACK_F77:
  case SPACK_FC:
    store_delimited(getenv("SPACK_TARGET_ARGS"), ' ', &s->strings,
                    &s->spack_compiler_flags);
    if (s->mode == SPACK_MODE_CCLD)
      store_delimited(getenv("SPACK_LDFLAGS"), ' ', &s->strings,
                      &s->spack_compiler_flags);
    store_delimited_flags(getenv("SPACK_INCLUDE_DIRS"), ':', "-I", &s->strings,
                          &s->spack_include_flags);
    break;
  }
}

// The exec* + posix_spawn calls we wrap

__attribute__((visibility("default"))) int
execve(const char *path, char *const *argv, char *const *envp) {
  struct state_t s;
  enum executable_t type = compiler_type(get_filename(path));

  if (type != SPACK_NONE) {
    path = override_path(type);
    arg_parse_init(&s);
    if (type != SPACK_LD)
      parse_compile_mode(argv, &s);
    parse_spack_env(&s, type);
    parse_argv(argv, &s, type);
    argv = arg_parse_finish(path, &s);
  }

  typeof(execve) *real = dlsym(RTLD_NEXT, "execve");
  return real(path, argv, envp);
}

__attribute__((visibility("default"))) int
execvpe(const char *file, char *const *argv, char *const *envp) {
  struct state_t s;
  enum executable_t type = compiler_type(get_filename(file));

  if (type != SPACK_NONE) {
    file = override_path(type);
    arg_parse_init(&s);
    if (type != SPACK_LD)
      parse_compile_mode(argv, &s);
    parse_spack_env(&s, type);
    parse_argv(argv, &s, type);
    argv = arg_parse_finish(file, &s);
  }

  for (int i = 0; envp[i]; i++)
    putenv(envp[i]);

  typeof(execvpe) *real = dlsym(RTLD_NEXT, "execvp");
  return real(file, argv, environ);
}

__attribute__((visibility("default"))) int
posix_spawn(pid_t *pid, const char *path,
            const posix_spawn_file_actions_t *file_actions,
            const posix_spawnattr_t *attrp, char *const *argv,
            char *const *envp) {
  struct state_t s;
  enum executable_t type = compiler_type(get_filename(path));

  if (type != SPACK_NONE) {
    path = override_path(type);
    arg_parse_init(&s);
    if (type != SPACK_LD)
      parse_compile_mode(argv, &s);
    parse_spack_env(&s, type);
    parse_argv(argv, &s, type);
    argv = arg_parse_finish(path, &s);
  }
  typeof(posix_spawn) *real = dlsym(RTLD_NEXT, "posix_spawn");
  return real(pid, path, file_actions, attrp, argv, envp);
}

// Fallback to execve / execvpe

__attribute__((visibility("default"))) int execl(const char *path,
                                                 const char *arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
  copy_args(argv, arg0, &ap);
  va_end(ap);
  return execve(path, argv, environ);
}

__attribute__((visibility("default"))) int execlp(const char *file,
                                                  const char *arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
  copy_args(argv, arg0, &ap);
  va_end(ap);
  return execvpe(file, argv, environ);
}

__attribute__((visibility("default"))) int execle(const char *path,
                                                  const char *arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
  copy_args(argv, arg0, &ap);
  char **env = va_arg(ap, char **);
  va_end(ap);
  return execve(path, argv, env);
}

__attribute__((visibility("default"))) int execv(const char *path,
                                                 char *const *argv) {
  return execve(path, argv, environ);
}

__attribute__((visibility("default"))) int execvp(const char *file,
                                                  char *const *argv) {
  return execvpe(file, argv, environ);
}
