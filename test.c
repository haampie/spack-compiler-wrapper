#include "spack-compiler-wrapper.h"

#include <stdio.h>

int main() {
    string_table_t st;
    arg_list_t argv;

    string_table_init(&st);
    arg_list_init(&argv);

    arg_list_push(&argv, string_table_store(&st, "hello world"));
    arg_list_push(&argv, string_table_store(&st, "another string"));

    printf("%s\n", st->arr + argv.argv[0]);
    printf("%s\n", st-> argv.argv[1]);
}
