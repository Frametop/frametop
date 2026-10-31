/* conf-test.py's C side of ft_conf.h: prints what it parses, values in hex.
 *   dump lines FILE      per line: ok NAME HEX | skip | bad
 *   dump get FILE KEY    1 HEX | 0          (ft_conf_get)
 *   dump setting KEY     1 HEX | 0          (ft_conf_setting: FT_KEY, then $HOME's conf)
 *   dump num TEXT        1 %.17g | 0        (ft_conf_number) */
#include "../ft_conf.h"

static void hex(const char *s)
{
    for (; *s; s++)
        printf("%02x", (unsigned char)*s);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "lines")) {
        FILE *f = fopen(argv[2], "r");
        char *line = NULL;
        size_t cap = 0;
        long len;
        if (!f)
            return 2;
        while ((len = ft_conf_getline(f, &line, &cap)) >= 0) {
            char name[4096], value[4096];
            const int r = ft_conf_parse_line(line, (size_t)len, name, sizeof name, value, sizeof value, NULL);
            if (r > 0) {
                printf("ok\t%s\t", name);
                hex(value);
                printf("\n");
            } else {
                printf(r == 0 ? "skip\n" : "bad\n");
            }
        }
        free(line);
        fclose(f);
        return 0;
    }
    if ((argc == 4 && !strcmp(argv[1], "get")) || (argc == 3 && !strcmp(argv[1], "setting"))) {
        char out[4096];
        const int r = argc == 4 ? ft_conf_get(argv[2], argv[3], out, sizeof out) : ft_conf_setting(argv[2], out, sizeof out);
        printf("%d\t", r);
        hex(out);
        printf("\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "num")) {
        double x;
        if (ft_conf_number(argv[2], &x))
            printf("1\t%.17g\n", x);
        else
            printf("0\n");
        return 0;
    }
    return 2;
}
