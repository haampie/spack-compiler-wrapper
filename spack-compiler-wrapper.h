#include <stddef.h>

typedef struct string_table_t {
    char *arr;
    size_t n;
    size_t capacity;
} string_table_t;

typedef struct arg_list_t {
    char ** argv;
    size_t n;
    size_t capacity;
} arg_list_t;

void string_table_init(struct string_table_t *t);
void string_table_reserve(struct string_table_t *t, size_t n);
size_t string_table_store(struct string_table_t *t, char const *str);

void arg_list_init(struct arg_list_t *t);
void arg_list_reserve(struct arg_list_t *t);
size_t arg_list_push(struct arg_list_t *t, char *arg);

