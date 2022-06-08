#define _GNU_SOURCE 1
#define SPACK_PATH_MAX 1024

#include <alloca.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum executable_t { SPACK_CC, SPACK_CXX, SPACK_FC, SPACK_F77, SPACK_LD, SPACK_NONE };

enum mode_t {
    SPACK_MODE_CCLD,     // preprocess, compile, assemble, link
    SPACK_MODE_CC,       // preprocess, compile, assemble (-c)
    SPACK_MODE_AS,       // preprocess, compile (-S)
    SPACK_MODE_CPP,      // preprocess (-E)
    SPACK_MODE_VERSION,  // -v, -V, --version, -dumpversion, -dumpmachine
    SPACK_MODE_INTERNAL, // e.g. clang -cc1
};

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

struct state_t {
    enum executable_t type;
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

    struct offset_list_t env;

    int has_ccache;
    size_t offset_ccache;
    size_t offset_compiler_or_linker;
};

struct new_args {
    char *const *argv;
    char *const *env;
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
static const char *spack_fc[] = {"ftn",      "f90",       "fc",        "f95",
                                 "gfortran", "flang",     "armflang",  "ifort",
                                 "ifx",      "pgfortran", "nvfortran", "xlf90",
                                 "xlf90_r",  "nagfor",    "frt",       "amdflang"};

// SPACK_F77
static const char *spack_f77[] = {"f77", "xlf", "xlf_r", "pgf77", "amdflang"};

// SPACK_LD
static const char *spack_ld[] = {"ld", "ld.gold", "ld.lld", "ld.bfd", "ld.mold"};

static void string_table_init(struct string_table_t *t) {
    t->arr = NULL;
    t->n = 0;
    t->capacity = 0;
}

static void string_table_reserve(struct string_table_t *t, size_t n) {
    if (t->n + n <= t->capacity)
        return;
    t->capacity = 2 * (t->n + n);
    char *arr = realloc(t->arr, t->capacity * sizeof(char));
    if (arr == NULL)
        exit(1);
    t->arr = arr;
}

static size_t string_table_store(struct string_table_t *t, char const *str) {
    size_t n = strlen(str) + 1;
    string_table_reserve(t, n);
    size_t offset = t->n;
    memcpy(t->arr + offset, str, n);
    t->n += n;
    return offset;
}

static size_t string_table_store_n(struct string_table_t *t, char const *str,
                                   size_t n) {
    string_table_reserve(t, n);
    size_t offset = t->n;
    memcpy(t->arr + offset, str, n);
    t->n += n;
    return offset;
}

static size_t string_table_store_flag(struct string_table_t *t, char const *flag,
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

static size_t string_table_store_flag_n(struct string_table_t *t, char const *flag,
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

static void offset_list_init(struct offset_list_t *t) {
    t->offsets = NULL;
    t->n = 0;
    t->capacity = 0;
}

static void offset_list_reserve(struct offset_list_t *t) {
    if (t->n < t->capacity)
        return;
    t->capacity = 2 * (t->n + 1);
    size_t *arr = realloc(t->offsets, t->capacity * sizeof(size_t));
    if (arr == NULL)
        exit(1);
    t->offsets = arr;
}

static void offset_list_push(struct offset_list_t *t, size_t offset) {
    offset_list_reserve(t);
    t->offsets[t->n++] = offset;
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
    default:
        return NULL;
    }
}

static const char *override_path(enum executable_t type) {
    char const *var = get_spack_variable(type);
    char const *path = getenv(var);
    if (path)
        return path;
    fprintf(stderr, "%s not set\n", var);
    exit(1);
}

static void arg_parse_init(struct state_t *s) {
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

    offset_list_init(&s->env);
    s->has_ccache = 0;
}

// re-assemble the command line arguments
static char *const *arg_parse_finish(struct state_t const *s) {
    size_t n = s->spack_compiler_flags.n + s->isystem_include_flags.n +
               s->include_flags.n + s->spack_include_flags.n +
               s->isystem_system_include_flags.n + s->system_include_flags.n +
               s->lib_flags.n + s->spack_lib_flags.n + s->system_lib_flags.n +
               s->rpath_flags.n + s->spack_rpath_flags.n + s->system_rpath_flags.n +
               s->other_flags.n;
    if (s->has_ccache)
        ++n;
    char **argv = malloc((n + 2) * sizeof(char *));

    size_t i = 0;

    if (s->has_ccache)
        argv[i++] = s->strings.arr + s->offset_ccache;

    argv[i++] = s->strings.arr + s->offset_compiler_or_linker;

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

    argv[i] = NULL;
    return argv;
}

// create env
static char *const *env_finish(struct state_t const *s) {
    char **env = malloc((s->env.n + 1) * sizeof(char *));
    for (size_t j = 0; j < s->env.n; ++j)
        env[j] = s->strings.arr + s->env.offsets[j];
    env[s->env.n] = NULL;
    return env;
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
    s->mode = SPACK_MODE_CCLD;
    for (size_t j = 0; argv[j] != NULL; ++j) {
        char *arg = argv[j];

        // Make sure this is a flag.
        if (arg[0] != '-' || arg[1] == '\0')
            continue;

        ++arg;

        // Single character flags
        if (arg[1] == '\0') {
            switch (*arg) {
            case 'c':
                s->mode = s->mode > SPACK_MODE_CC ? s->mode : SPACK_MODE_CC;
                break;
            case 'S':
                s->mode = s->mode > SPACK_MODE_AS ? s->mode : SPACK_MODE_AS;
                break;
            case 'E':
                s->mode = s->mode > SPACK_MODE_CPP ? s->mode : SPACK_MODE_CPP;
                break;
            case 'v':
            case 'V':
                s->mode = s->mode > SPACK_MODE_VERSION ? s->mode : SPACK_MODE_VERSION;
                return;
            }
        } else if (strcmp(arg, "cc1") == 0) {
            // We don't wanna intercept Clang's -cc1 "internal" calls.
            s->mode = SPACK_MODE_INTERNAL;
            return;
        } else if (strcmp(arg, "-version") == 0 || strcmp(arg, "dumpversion") == 0) {
            // --version and -dumpversion; we don't intercept them.
            s->mode = SPACK_MODE_VERSION;
            return;
        }
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
            offset_list_push(system_path(c) ? &s->system_rpath_flags : &s->rpath_flags,
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

static void parse_argv(char *const *argv, struct state_t *s) {
    switch (s->type) {
    case SPACK_LD:
        parse_ld(argv, s);
        break;
    case SPACK_CC:
    case SPACK_CXX:
    case SPACK_FC:
    case SPACK_F77:
        parse_cc(argv, s);
        break;
    default:
        break;
    }
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
        if (len > 0) {
            // Copy the delimeter and replace it with \0.
            offset_list_push(list,
                             string_table_store_flag_n(strings, flag, p, len + 1));
            strings->arr[strings->n - 1] = '\0';
        }
        if (end == NULL)
            return;
        p = end + 1;
    }
}

static void store_delimited(char const *str, char delim, struct string_table_t *strings,
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

static void parse_spack_env(struct state_t *s) {
    switch (s->type) {
    case SPACK_LD:
        store_delimited_flags(getenv("SPACK_LINK_DIRS"), ':', "-L", &s->strings,
                              &s->spack_lib_flags);
        store_delimited_flags(getenv("SPACK_COMPILER_EXTRA_RPATHS"), ':', "-L",
                              &s->strings, &s->spack_lib_flags);
        store_delimited_flags(getenv("SPACK_RPATH_DIRS"), ':', "-rpath=", &s->strings,
                              &s->spack_rpath_flags);
        store_delimited_flags(getenv("SPACK_COMPILER_EXTRA_RPATHS"), ':',
                              "-rpath=", &s->strings, &s->spack_rpath_flags);
        store_delimited_flags(getenv("SPACK_COMPILER_IMPLICIT_RPATHS"), ':',
                              "-rpath=", &s->strings, &s->spack_rpath_flags);
        // TODO: improve LDLIBS?
        store_delimited_flags(getenv("SPACK_LDLIBS"), ' ', "-l", &s->strings,
                              &s->spack_lib_flags);
        break;
    case SPACK_CC:
        store_delimited(getenv("SPACK_CPPFLAGS"), ' ', &s->strings,
                        &s->spack_compiler_flags);
        store_delimited(getenv("SPACK_CFLAGS"), ' ', &s->strings,
                        &s->spack_compiler_flags);
        break;
    case SPACK_CXX:
        store_delimited(getenv("SPACK_CPPFLAGS"), ' ', &s->strings,
                        &s->spack_compiler_flags);
        store_delimited(getenv("SPACK_CXXFLAGS"), ' ', &s->strings,
                        &s->spack_compiler_flags);
        break;
    case SPACK_F77:
    case SPACK_FC:
        store_delimited(getenv("SPACK_FFLAGS"), ' ', &s->strings,
                        &s->spack_compiler_flags);
        store_delimited(getenv("SPACK_CPPFLAGS"), ' ', &s->strings,
                        &s->spack_compiler_flags);
        break;
    default:
        break;
    }
    switch (s->type) {
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
    default:
        break;
    }
}

static void copy_env(struct string_table_t *strings, struct offset_list_t *env_offsets,
                     char *const *envp) {
    for (char *const *env = envp; *env != NULL; ++env)
        offset_list_push(env_offsets, string_table_store(strings, *env));
}

static struct new_args rewrite_args_and_env(char *const *argv, char *const *envp,
                                            struct state_t *s) {
    struct new_args args;
    arg_parse_init(s);

    // Store the actual compiler
    s->offset_compiler_or_linker =
        string_table_store(&s->strings, override_path(s->type));

    // Maybe store ccache path.
    if (s->type == SPACK_CC || s->type == SPACK_CXX) {
        char const *ccache = getenv("SPACK_CCACHE_BINARY");
        if (ccache != NULL) {
            s->has_ccache = 1;
            s->offset_ccache = string_table_store(&s->strings, ccache);
        } else {
            s->has_ccache = 0;
        }
    }

    // Copy the environment variables
    copy_env(&s->strings, &s->env, envp);

    // Store SPACK_CC/LD_DONE to avoid recursive wrapping.
    if (s->type == SPACK_CC || s->type == SPACK_CXX || s->type == SPACK_F77 ||
        s->type == SPACK_FC)
        offset_list_push(&s->env, string_table_store(&s->strings, "SPACK_CC_DONE=1"));

    if (s->type == SPACK_LD)
        offset_list_push(&s->env, string_table_store(&s->strings, "SPACK_LD_DONE=1"));

    parse_spack_env(s);
    parse_argv(argv, s);

    args.argv = arg_parse_finish(s);
    args.env = env_finish(s);

    char const *test_command = getenv("SPACK_TEST_COMMAND");
    if (test_command == NULL) {
        return args;
    } else if (strcmp(test_command, "dump-args") == 0) {
        for (char *const *arg = args.argv; *arg != NULL; ++arg)
            puts(*arg);
        exit(0);
    } else if (strncmp(test_command, "dump-env-", 9) == 0) {
        test_command += 9;
        size_t needle_length = strlen(test_command);
        for (char *const *env = environ; *env != NULL; ++env) {
            if (strncmp(*env, test_command, needle_length) == 0 &&
                (*env)[needle_length] == '=') {
                puts(*env);
                exit(0);
            }
        }
        exit(1);
    } else {
        return args;
    }
}

static int should_intercept(const char *path, char *const *argv, struct state_t *s) {
    // Disable if not a compiler or linker
    s->type = compiler_type(get_filename(path));
    if (s->type == SPACK_NONE)
        return 0;

    // Disable if we already wrapped it
    if (s->type == SPACK_LD && getenv("SPACK_LD_DONE") != NULL)
        return 0;
    if ((s->type == SPACK_CC || s->type == SPACK_CXX || s->type == SPACK_F77 ||
         s->type == SPACK_FC) &&
        getenv("SPACK_CC_DONE") != NULL)
        return 0;

    // Quickly scan for clang -cc1 type of args; we shouldn't wrap those.
    parse_compile_mode(argv, s);

    return s->mode != SPACK_MODE_INTERNAL && s->mode != SPACK_MODE_VERSION;
}

static void maybe_debug(struct state_t const *s, const char *path, char *const *args_in,
                        char *const *args_out) {
    if (getenv("SPACK_DEBUG") == NULL)
        return;
    char *dir = getenv("SPACK_DEBUG_LOG_DIR");
    char *id = getenv("SPACK_DEBUG_LOG_ID");
    if (dir == NULL || id == NULL)
        return;

    // SPACK_DEBUG_LOG_DIR/spack-cc-$SPACK_DEBUG_LOG_ID.in.log
    char path_in[SPACK_PATH_MAX];

    // SPACK_DEBUG_LOG_DIR/spack-cc-$SPACK_DEBUG_LOG_ID.out.log
    char path_out[SPACK_PATH_MAX];
    size_t dir_len = strlen(dir);
    size_t id_len = strlen(id);
    size_t in_length = dir_len + 10 + id_len + 7;
    size_t out_length = dir_len + 10 + id_len + 8;
    if (out_length + 1 > SPACK_PATH_MAX)
        return;
    memcpy(path_in, dir, dir_len);
    memcpy(path_in + dir_len, "/spack-cc-", 10);
    memcpy(path_in + dir_len + 10, id, id_len);
    memcpy(path_in + dir_len + 10 + id_len, ".in.log", 7);
    path_in[in_length] = '\0';
    memcpy(path_out, dir, dir_len);
    memcpy(path_out + dir_len, "/spack-cc-", 10);
    memcpy(path_out + dir_len + 10, id, id_len);
    memcpy(path_out + dir_len + 10 + id_len, ".out.log", 8);
    path_out[out_length] = '\0';

    FILE *in = fopen(path_in, "a");
    FILE *out = fopen(path_out, "a");
    if (in == NULL || out == NULL)
        return;

    char const *mode =
        s->type == SPACK_LD
            ? "[ld] "
            : s->mode == SPACK_MODE_AS
                  ? "[as] "
                  : s->mode == SPACK_MODE_CC
                        ? "[cc] "
                        : s->mode == SPACK_MODE_CCLD ? "[ccld] " : "[cpp]";
    fputs(mode, in);
    for (char *const *arg_in = args_in; *arg_in != NULL; ++arg_in) {
        fputs(*arg_in, in);
        fputc(' ', in);
    }
    fputc('\n', in);
    fclose(in);

    fputs(mode, out);
    for (char *const *arg_out = args_out; *arg_out != NULL; ++arg_out) {
        fputs(*arg_out, out);
        fputc(' ', out);
    }
    fputc('\n', out);
    fclose(out);
}

// The exec* + posix_spawn calls we wrap

__attribute__((visibility("default"))) int execve(const char *path, char *const *argv,
                                                  char *const *envp) {
    struct state_t s;
    typeof(execve) *next = dlsym(RTLD_NEXT, "execve");
    if (!should_intercept(path, argv, &s))
        return next(path, argv, envp);
    struct new_args args = rewrite_args_and_env(argv, envp, &s);
    maybe_debug(&s, path, argv, args.argv);
    return next(args.argv[0], args.argv, args.env);
}

__attribute__((visibility("default"))) int execvpe(const char *file, char *const *argv,
                                                   char *const *envp) {
    struct state_t s;
    typeof(execvpe) *next = dlsym(RTLD_NEXT, "execvp");
    if (!should_intercept(file, argv, &s))
        return next(file, argv, envp);
    struct new_args args = rewrite_args_and_env(argv, envp, &s);
    maybe_debug(&s, file, argv, args.argv);
    return next(args.argv[0], args.argv, args.env);
}

__attribute__((visibility("default"))) int
posix_spawn(pid_t *pid, const char *path,
            const posix_spawn_file_actions_t *file_actions,
            const posix_spawnattr_t *attrp, char *const *argv, char *const *envp) {
    struct state_t s;
    typeof(posix_spawn) *next = dlsym(RTLD_NEXT, "posix_spawn");
    if (!should_intercept(path, argv, &s))
        return next(pid, path, file_actions, attrp, argv, envp);
    struct new_args args = rewrite_args_and_env(argv, envp, &s);
    maybe_debug(&s, path, argv, args.argv);
    return next(pid, args.argv[0], file_actions, attrp, args.argv, args.env);
}

// Fallback to execve / execvpe

__attribute__((visibility("default"))) int execl(const char *path, const char *arg0,
                                                 ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
    copy_args(argv, arg0, &ap);
    va_end(ap);
    return execve(path, argv, environ);
}

__attribute__((visibility("default"))) int execlp(const char *file, const char *arg0,
                                                  ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
    copy_args(argv, arg0, &ap);
    va_end(ap);
    return execvpe(file, argv, environ);
}

__attribute__((visibility("default"))) int execle(const char *path, const char *arg0,
                                                  ...) {
    va_list ap;
    va_start(ap, arg0);
    char **argv = alloca((count_args(&ap) + 2) * sizeof(char *));
    copy_args(argv, arg0, &ap);
    char **env = va_arg(ap, char **);
    va_end(ap);
    return execve(path, argv, env);
}

__attribute__((visibility("default"))) int execv(const char *path, char *const *argv) {
    return execve(path, argv, environ);
}

__attribute__((visibility("default"))) int execvp(const char *file, char *const *argv) {
    return execvpe(file, argv, environ);
}
