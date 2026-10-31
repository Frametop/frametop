#include "rendernode.h"

#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace rendernode {

namespace {
int g_fd = -1, g_users = 0;
gbm_device *g_gbm = nullptr;
}  // namespace

gbm_device *Acquire(const char *who) {
    if (!g_gbm) {
        g_fd = open(FT_RENDER_NODE, O_RDWR | O_CLOEXEC);
        if (g_fd < 0) return std::perror((std::string(who) + ": " FT_RENDER_NODE).c_str()), nullptr;
        g_gbm = gbm_create_device(g_fd);
        if (!g_gbm) {
            std::fprintf(stderr, "%s: no GBM device on " FT_RENDER_NODE "\n", who);
            close(g_fd), g_fd = -1;
            return nullptr;
        }
    }
    ++g_users;
    return g_gbm;
}

void Release() {
    if (g_users <= 0 || --g_users > 0) return;
    gbm_device_destroy(g_gbm);
    close(g_fd);
    g_gbm = nullptr, g_fd = -1;
}

}  // namespace rendernode
