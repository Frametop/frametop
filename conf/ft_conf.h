/* ft_conf.h: ~/.config/frametop.conf for C and C++, the grammar of docs/config.md, the same as
 * conf/ft_conf.py (conf/test/ checks all three against bash on shared vectors). Header-only.
 *
 * A line is blank, a comment, or NAME=value with an optional "# comment" after whitespace. The
 * value is literal: bare characters, 'single quotes' or "double quotes" without $ ` \, side by
 * side. Any other line (a second word, $(...), ;, a backslash, spaces around the =) is one bash
 * reads differently or runs: it's left out with a warning on stderr, and the key keeps its
 * default. The last line of a name wins, as when bash sources the file.
 *
 * C:   char v[64]; if (ft_conf_setting("HANDS_CAMERAS", v, sizeof v)) ...  (FT_<name> first)
 *      double x; if (ft_conf_number(v, &x)) ...                           (strict: no garbage = 0)
 * C++: auto conf = ft_conf::Read();  ft_conf::Double(conf, "POINTER_IDLE", 30);
 *      ft_conf::Setting("HANDS_MATTE", "circle");                         (FT_<name> first)
 */
#ifndef FT_CONF_H
#define FT_CONF_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Names a setting can't have (ft_conf.py's RESERVED): bash's read-only ones, and the ones that
 * change how the session's shell runs; every BASH_* name too. */
static const char *const ft_conf_reserved[] = {"BASHOPTS", "CDPATH", "ENV", "EUID", "EXECIGNORE", "FUNCNEST",
                                               "GLOBIGNORE", "HOME", "IFS", "OPTIND", "PATH", "POSIXLY_CORRECT",
                                               "PPID", "PS4", "SHELLOPTS", "TMOUT", "UID", NULL};

/* One line (len bytes, no newline): 1 = an assignment (name and value copied, NUL-terminated),
 * 0 = blank or a comment, -1 = outside the grammar (*why says why) or too long for the buffers. */
static inline int ft_conf_parse_line(const char *line, size_t len, char *name, size_t name_n,
                                     char *value, size_t value_n, const char **why)
{
    size_t i = 0, start, nv = 0;
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = "";
    for (size_t k = 0; k < len; k++)
        if (line[k] == '\r' || line[k] == '\0' || line[k] == '\n') {
            *why = "a carriage return, newline or NUL in the line";
            return -1;
        }
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i == len || line[i] == '#')
        return 0;
    start = i;
    while (i < len && ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') ||
                       (line[i] >= '0' && line[i] <= '9') || line[i] == '_'))
        i++;
    if (i == start || (line[start] >= '0' && line[start] <= '9') || i == len || line[i] != '=') {
        *why = "not NAME=value (no spaces around the =)";
        return -1;
    }
    if (i - start + 1 > name_n) {
        *why = "the name is too long";
        return -1;
    }
    memcpy(name, line + start, i - start);
    name[i - start] = 0;
    for (size_t k = start; k < i; k++)
        if (line[k] >= 'a' && line[k] <= 'z') {
            *why = "a lower-case name (the session script's own variables): settings are upper-case";
            return -1;
        }
    for (const char *const *r = ft_conf_reserved; *r; r++)
        if (!strcmp(name, *r) || !strncmp(name, "BASH_", 5)) {
            *why = "a name bash keeps for itself (read-only, or how the shell runs)";
            return -1;
        }
    i++;
#define FT_CONF_PUT(c)                       \
    do {                                     \
        if (nv + 1 >= value_n) {             \
            *why = "the value is too long";  \
            return -1;                       \
        }                                    \
        value[nv++] = (c);                   \
    } while (0)
    while (i < len && line[i] != ' ' && line[i] != '\t') {
        const char c = line[i];
        if (c == '\'') {
            size_t j = i + 1;
            while (j < len && line[j] != '\'')
                FT_CONF_PUT(line[j++]);
            if (j == len) {
                *why = "a ' with no closing '";
                return -1;
            }
            i = j + 1;
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < len && line[j] != '"') {
                if (line[j] == '$' || line[j] == '`' || line[j] == '\\') {
                    *why = "$, ` or \\ inside \"double quotes\" (bash expands them): use 'single quotes'";
                    return -1;
                }
                FT_CONF_PUT(line[j++]);
            }
            if (j == len) {
                *why = "a \" with no closing \"";
                return -1;
            }
            i = j + 1;
        } else if (strchr("'\"\\`$;&|<>()", c)) {
            *why = "shell syntax outside quotes: quote the value";
            return -1;
        } else {
            FT_CONF_PUT(c);
            i++;
        }
    }
#undef FT_CONF_PUT
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i < len && line[i] != '#') {
        *why = "more than one word after the = (bash runs the rest as a command): quote the value";
        return -1;
    }
    if (value_n)
        value[nv] = 0;
    return 1;
}

/* The next line of f into *buf (grown as needed), without its newline: its length, or -1 at the
 * end. NULs stay in (the parser refuses the line). */
static inline long ft_conf_getline(FILE *f, char **buf, size_t *cap)
{
    size_t n = 0;
    int c;
    while ((c = getc(f)) != EOF && c != '\n') {
        if (n + 1 >= *cap) {
            size_t want = *cap ? *cap * 2 : 256;
            char *b = (char *)realloc(*buf, want);
            if (!b)
                return -1;
            *buf = b, *cap = want;
        }
        (*buf)[n++] = (char)c;
    }
    if (c == EOF && n == 0)
        return -1;
    if (!*buf) {
        *buf = (char *)malloc(1);
        if (!*buf)
            return -1;
        *cap = 1;
    }
    (*buf)[n] = 0;
    return (long)n;
}

/* Is this line about key (its first word is key, before an = or a space)? For the warning. */
static inline int ft_conf_line_is(const char *line, size_t len, const char *key)
{
    size_t i = 0, k = strlen(key);
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    return len - i >= k && !memcmp(line + i, key, k) &&
           (i + k == len || line[i + k] == '=' || line[i + k] == ' ' || line[i + k] == '\t');
}

/* key's value in the file at path (the last valid line of it) into out: 1, or 0 when it isn't
 * there (out ""). A bad line of key's is warned on stderr and left out. */
static inline int ft_conf_get(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char *line = NULL, *name, *value;
    size_t cap = 0, no = 0;
    long len;
    int found = 0;
    if (n)
        out[0] = 0;
    if (!f)
        return 0;
    while ((len = ft_conf_getline(f, &line, &cap)) >= 0) {
        const char *why;
        no++;
        name = (char *)malloc((size_t)len + 1), value = (char *)malloc((size_t)len + 1);
        if (!name || !value) {
            free(name), free(value);
            break;
        }
        const int r = ft_conf_parse_line(line, (size_t)len, name, (size_t)len + 1, value, (size_t)len + 1, &why);
        if (r == 1 && !strcmp(name, key)) {
            if (strlen(value) < n) {
                memcpy(out, value, strlen(value) + 1);
                found = 1;
            } else {
                fprintf(stderr, "frametop.conf:%zu: %s left out: the value is too long\n", no, key);
            }
        } else if (r < 0 && ft_conf_line_is(line, (size_t)len, key)) {
            fprintf(stderr, "frametop.conf:%zu: %s left out: %s\n", no, key, why);
        }
        free(name), free(value);
    }
    free(line);
    fclose(f);
    if (!found && n)
        out[0] = 0;
    return found;
}

/* ~/.config/frametop.conf into buf. */
static inline const char *ft_conf_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/frametop.conf", home ? home : "");
    return buf;
}

/* FT_<key> from the environment, else frametop.conf's key, into out: 1, or 0 when neither is set. */
static inline int ft_conf_setting(const char *key, char *out, size_t n)
{
    char env[128], path[1024];
    snprintf(env, sizeof(env), "FT_%s", key);
    const char *v = getenv(env);
    if (v) {
        snprintf(out, n, "%s", v);
        return 1;
    }
    return ft_conf_get(ft_conf_path(path, sizeof(path)), key, out, n);
}

/* text as a number: [+-]digits[.digits][e[+-]digits], finite. 1 and *out, or 0 (garbage, hex,
 * inf, empty), where atof would have said 0. */
static inline int ft_conf_number(const char *text, double *out)
{
    const char *p = text;
    int digits = 0;
    if (*p == '+' || *p == '-')
        p++;
    while (*p >= '0' && *p <= '9')
        p++, digits++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9')
            p++, digits++;
    }
    if (!digits)
        return 0;
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        if (*e == '+' || *e == '-')
            e++;
        if (!(*e >= '0' && *e <= '9'))
            return 0;
        while (*e >= '0' && *e <= '9')
            e++;
        p = e;
    }
    if (*p)
        return 0;
    const double x = strtod(text, NULL);
    if (!isfinite(x))
        return 0;
    *out = x;
    return 1;
}

#ifdef __cplusplus
#include <map>
#include <string>
#include <vector>

namespace ft_conf {

inline std::string Path()
{
    char buf[1024];
    return ft_conf_path(buf, sizeof(buf));
}

// Every valid setting of the file; {} when it's missing. Bad lines are left out, and warned on
// stderr: all of them, or only warn_key's.
inline std::map<std::string, std::string> ReadWarning(const std::string &path, const char *warn_key)
{
    std::map<std::string, std::string> conf;
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return conf;
    char *line = nullptr;
    size_t cap = 0, no = 0;
    long len;
    while ((len = ft_conf_getline(f, &line, &cap)) >= 0) {
        no++;
        std::vector<char> name(size_t(len) + 1), value(size_t(len) + 1);
        const char *why;
        const int r = ft_conf_parse_line(line, size_t(len), name.data(), name.size(), value.data(), value.size(), &why);
        if (r == 1) {
            conf[name.data()] = value.data();
        } else if (r < 0 && (!warn_key || ft_conf_line_is(line, size_t(len), warn_key))) {
            std::string head(line, size_t(len));
            head = head.substr(0, head.find('='));
            head.erase(0, head.find_first_not_of(" \t"));
            head.erase(head.find_last_not_of(" \t") + 1);
            std::fprintf(stderr, "frametop.conf:%zu: %s left out: %s\n", no, head.substr(0, 40).c_str(), why);
        }
    }
    std::free(line);
    std::fclose(f);
    return conf;
}

// Every valid setting of the file (default: ~/.config/frametop.conf); {} when it's missing. Each
// bad line is warned on stderr and left out.
inline std::map<std::string, std::string> Read(const std::string &path = Path()) { return ReadWarning(path, nullptr); }

// FT_<name> from the environment, else the file's name, else def.
inline std::string Setting(const char *name, const char *def)
{
    const std::string env = std::string("FT_") + name;
    if (const char *v = std::getenv(env.c_str())) return v;
    const auto conf = ReadWarning(Path(), name);   // only name's bad lines: callers ask for many
    const auto it = conf.find(name);
    return it == conf.end() ? def : it->second;
}

// key as a number, or fallback when it's unset or empty; a value that isn't one (atof's 0 before)
// warns on stderr and gives fallback too.
inline double Double(const std::map<std::string, std::string> &c, const char *key, double fallback)
{
    const auto it = c.find(key);
    if (it == c.end() || it->second.empty()) return fallback;
    double x;
    if (ft_conf_number(it->second.c_str(), &x)) return x;
    std::fprintf(stderr, "frametop.conf: %s=%s isn't a number; using %g\n", key, it->second.c_str(), fallback);
    return fallback;
}

}  // namespace ft_conf
#endif

#endif
