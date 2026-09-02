#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) {
        fprintf(stderr, "SW-Doom: cannot resolve executable path\n");
        return 1;
    }

    char exe[PATH_MAX];
    if (!realpath(raw, exe)) {
        perror("SW-Doom: realpath");
        return 1;
    }

    char *slash = strrchr(exe, '/');
    if (!slash) {
        fprintf(stderr, "SW-Doom: bad executable path\n");
        return 1;
    }
    *slash = '\0';

    char contents[PATH_MAX];
    snprintf(contents, sizeof(contents), "%s", exe);
    slash = strrchr(contents, '/');
    if (!slash) {
        fprintf(stderr, "SW-Doom: bad Contents path\n");
        return 1;
    }
    *slash = '\0';

    char engine[PATH_MAX];
    char iwad[PATH_MAX];
    snprintf(engine, sizeof(engine), "%s/swdoom", exe);
    snprintf(iwad, sizeof(iwad), "%s/Resources/freedoom1.wad", contents);

    /* Absolute IWAD: IdentifyVersion uses the -iwad path as given; Finder cwd is not the repo. */
    int n = argc + 2;
    char **nargv = calloc((size_t)n + 1, sizeof(char *));
    if (!nargv)
        return 1;
    nargv[0] = "swdoom";
    nargv[1] = "-iwad";
    nargv[2] = iwad;
    for (int i = 1; i < argc; i++)
        nargv[i + 2] = argv[i];

    execv(engine, nargv);
    perror("SW-Doom: execv swdoom");
    free(nargv);
    return 1;
}
