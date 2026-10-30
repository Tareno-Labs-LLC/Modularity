#include <libtcc.h>
#include <stdio.h>
#include <string.h>

static int host_context(int value) { return value + 7; }
static void diagnostic(void *unused, const char *message) {
    (void)unused;
    fprintf(stderr, "%s\n", message);
}
int main(int argc, char **argv) {
    TCCState *state = tcc_new();
    if (!state) return 1;
    tcc_set_lib_path(state, TCC_PROBE_RUNTIME);
    tcc_set_error_func(state, NULL, diagnostic);
    tcc_set_options(state, "-nostdlib");
    const int object = argc == 2;
    if (tcc_set_output_type(state, object ? TCC_OUTPUT_OBJ : TCC_OUTPUT_MEMORY) < 0) return 1;
    if (!object) tcc_add_symbol(state, "host_context", (const void *)host_context);
    if (tcc_compile_string(state,
        "extern int host_context(int);\n"
        "struct Context { int value; };\n"
        "int script(struct Context *ctx) { return host_context(ctx->value); }\n") < 0) return 1;
    if (object) {
        if (tcc_output_file(state, argv[1]) < 0) return 1;
        puts("[PASS] TinyCC produced a target relocatable C object");
    } else {
        struct Context { int value; } ctx = {35};
        if (tcc_relocate(state) < 0) return 1;
        int (*script)(struct Context *) = (int (*)(struct Context *))tcc_get_symbol(state, "script");
        if (!script || script(&ctx) != 42) return 1;
        puts("[PASS] TinyCC C script called a host function with a context pointer");
    }
    tcc_delete(state);
    return 0;
}
