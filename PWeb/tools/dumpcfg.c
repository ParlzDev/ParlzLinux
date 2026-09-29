#include "pweb.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    cfg_t *c = NULL;
    if (cfg_parse(argv[1], &c) != 0) {
        fprintf(stderr, "parse failed\n");
        return 1;
    }
    printf("nhosts=%d\n", c->nhosts);
    for (int i = 0; i < c->nhosts; i++) {
        const vhost_t *vh = &c->hosts[i];
        printf("host %d: port=%d root=[%s] nlocs=%d\n",
               i, vh->port, vh->root, vh->nlocs);
        for (int j = 0; j < vh->nlocs; j++) {
            const loc_t *lc = &vh->locs[j];
            printf("  loc %d: type=%d match=[%s] root=[%s] "
                   "cgi_script=[%s] proxypass=[%s]\n",
                   j, lc->type, lc->match, lc->root,
                   lc->cgi_script, lc->proxypass);
        }
    }
    cfg_free(c);
    return 0;
}
