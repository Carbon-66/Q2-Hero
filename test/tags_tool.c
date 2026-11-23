/* tags.c alone: tags_tool FILE... prints each file's title and artist, tab-separated */
#include <stdio.h>
#include "../src/hero.h"
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        char t[200], a[120];
        int ok = tags_read(argv[i], t, sizeof t, a, sizeof a);
        printf("%s\t%d\t%s\t%s\n", argv[i], ok, t, a);
    }
    return 0;
}
