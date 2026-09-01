#include <stdio.h>
#include <unistd.h>
#include "pidfile.h"

int pidfile_write(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return 0;
}

void pidfile_remove(const char *path)
{
    unlink(path); /* ignore errors: nothing to clean up if it's missing */
}
