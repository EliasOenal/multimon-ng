#define main gen_cli_main
#include "../gen.c"
#undef main

#include <sys/wait.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

int main(void)
{
    char dir[] = "/tmp/test_gen_flex_alloc.XXXXXX";
    char path[sizeof(dir) + 16];
    CHECK(mkdtemp(dir) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/output.raw", dir) > 0);
    char *cases[][12] = {
        {"gen-ng", "-t", "raw", "-f", "test", path, NULL},
        {"gen-ng", "-t", "raw", "-f", "test", "-G", "1000000:93", path, NULL},
        {"gen-ng", "-t", "raw", "-s", "1000", "-f", "test", path, NULL},
        {"gen-ng", "-t", "raw", "-f", "test", "-s", "1000", path, NULL}
    };
    int failed = 0;
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int fds[2], status;
        CHECK(pipe(fds) == 0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            close(fds[0]);
            CHECK(dup2(fds[1], STDERR_FILENO) == STDERR_FILENO);
            close(fds[1]);
            int argc = 0;
            while (cases[i][argc]) argc++;
            exit(gen_cli_main(argc, cases[i]));
        }
        close(fds[1]);
        char diagnostic[256] = {0};
        size_t used = 0;
        ssize_t count;
        while ((count = read(fds[0], diagnostic + used,
                             sizeof(diagnostic) - 1 - used)) > 0)
            used += (size_t)count;
        CHECK(count == 0);
        close(fds[0]);
        CHECK(waitpid(child, &status, 0) == child);
        struct stat st;
        int created = stat(path, &st) == 0;
        CHECK(created || errno == ENOENT);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_FAILURE ||
            strcmp(diagnostic, "gen_flex: out of memory\n") || created) {
            fprintf(stderr, "case %u: status=%d, output_created=%d, bytes=%lld, stderr=%s",
                    i, status, created, created ? (long long)st.st_size : 0, diagnostic);
            failed = 1;
        }
        if (created) CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(dir) == 0);
    CHECK(!failed);
    puts("FLEX allocation failure exits before output creation");
    return 0;
}
