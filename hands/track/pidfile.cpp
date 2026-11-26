#include "pidfile.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

bool pidfile_write(const std::string &path) {
    // its folder (the run dir) may not be there yet: ft-camd, which makes it, may still be starting
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) mkdir(path.substr(0, slash).c_str(), 0700);
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && !S_ISREG(st.st_mode)) return false;
    const std::string tmp = path + ".tmp" + std::to_string(getpid());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string text = std::to_string(getpid()) + "\n";
    const bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
    if (close(fd) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

void pidfile_remove(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "re");
    if (!f) return;
    long pid = 0;
    const bool read = std::fscanf(f, "%ld", &pid) == 1;
    std::fclose(f);
    if (read && pid == long(getpid())) unlink(path.c_str());
}
