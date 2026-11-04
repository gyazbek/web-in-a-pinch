// libFuzzer harness for the config file reader and proxy route parsing.
//
// Each input is written out as a config file and loaded with load_config, the same
// way wip loads wip.json at startup. Settings and routes are reset between inputs.
//
// Linux only (needs clang's libFuzzer). Build and run with tests/fuzz.sh.

#define main wip_main
#include "../wip.c"
#undef main

static char config_path[] = "/tmp/wip-fuzz-config-XXXXXX";
static wip_config_t defaults;

int LLVMFuzzerInitialize(int* argc, char*** argv) {
    (void)argc;
    (void)argv;
    int fd = mkstemp(config_path);
    if (fd < 0) abort();
    close(fd);
    defaults = config;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FILE* f = fopen(config_path, "wb");
    if (!f) abort();
    fwrite(data, 1, size, f);
    fclose(f);

    config = defaults;
    proxy_target_count = 0;
    load_config(config_path);

    // Route matching against whatever routes the input managed to create
    static const char* targets[] = { "/", "/api", "/api/x?y", "/apix", "//", "/a/b/c", "" };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        size_t prefix_len;
        find_proxy_target(targets[i], &prefix_len);
    }
    return 0;
}
