#include "usb_gadget_configfs.h"

#include "logging.h"
#include "product.h"
#include "usb_common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG(...) pik_log("ffs", __VA_ARGS__)

#define CONFIGFS_DIR "/sys/kernel/config"
#define CONFIGFS_CONFIG "/sys/kernel/config/usb_gadget/pik1/configs/c.1"
#define CONFIGFS_FUNCTIONS "/sys/kernel/config/usb_gadget/pik1/functions"

static bool write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool mkdir_p(const char *path) {
    char tmp[256];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool write_text(const char *path, const char *value) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s\n", value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return false;
    }

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write_all(fd, buf, (size_t)n);
    int saved = errno;
    close(fd);
    errno = saved;
    return ok;
}

static bool is_mountpoint(const char *path) {
    struct stat st, parent;
    char p[256];
    size_t len = strlen(path);
    if (len + 4 > sizeof(p)) return false;
    if (stat(path, &st) < 0) return false;
    snprintf(p, sizeof(p), "%s/..", path);
    if (stat(p, &parent) < 0) return false;
    return st.st_dev != parent.st_dev || st.st_ino == parent.st_ino;
}

static void try_modprobe(const char *name) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG("fork modprobe %s: %s", name, strerror(errno));
        return;
    }
    if (pid == 0) {
        execlp("modprobe", "modprobe", name, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        LOG("modprobe %s returned %d", name,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static bool ensure_configfs(void) {
    if (is_mountpoint(CONFIGFS_DIR)) return true;
    LOG("mounting configfs");
    if (mount("none", CONFIGFS_DIR, "configfs", 0, NULL) < 0 && errno != EBUSY) {
        LOG("mount configfs: %s", strerror(errno));
        return false;
    }
    return true;
}

static void unlink_if_exists(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (S_ISDIR(st.st_mode))
        (void)rmdir(path);
    else
        (void)unlink(path);
}

static void remove_matching(const char *dir, const char *prefix, bool dirs) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    size_t prefix_len = strlen(prefix);
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, prefix_len) != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (dirs)
            (void)rmdir(path);
        else
            (void)unlink(path);
    }
    closedir(d);
}

static bool unbind_gadget(void) {
    char current[256] = "";
    char path[256];
    snprintf(path, sizeof(path), "%s/UDC", PIK_CONFIGFS_GADGET);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        LOG("open %s: %s", path, strerror(errno));
        return false;
    }
    ssize_t n = read(fd, current, sizeof(current) - 1);
    if (n > 0 && current[0] && current[0] != '\n') {
        LOG("unbinding existing gadget from %.*s", (int)n, current);
        (void)lseek(fd, 0, SEEK_SET);
        (void)write(fd, "\n", 1);
    }
    close(fd);
    return true;
}

bool pik_ffs_prepare_gadget(void) {
    char path[256];
    char ffs_function[32];
    snprintf(ffs_function, sizeof(ffs_function), "ffs.%s", PIK1_FFS_NAME);

    LOG("loading USB gadget modules");
    try_modprobe("libcomposite");
    try_modprobe("usb_f_fs");

    if (!mkdir_p(CONFIGFS_DIR) || !ensure_configfs())
        return false;

    if (!mkdir_p(PIK_CONFIGFS_GADGET)) {
        LOG("mkdir %s: %s", PIK_CONFIGFS_GADGET, strerror(errno));
        return false;
    }
    if (!unbind_gadget())
        return false;

    if (is_mountpoint(PIK1_FFS_MOUNT)) {
        LOG("unmounting stale FunctionFS mount");
        if (umount2(PIK1_FFS_MOUNT, MNT_DETACH) < 0) {
            LOG("umount %s: %s", PIK1_FFS_MOUNT, strerror(errno));
            return false;
        }
    }

    remove_matching(CONFIGFS_CONFIG, "acm.", false);
    remove_matching(CONFIGFS_CONFIG, "ffs.", false);
    snprintf(path, sizeof(path), "%s/ffs", CONFIGFS_CONFIG);
    unlink_if_exists(path);
    remove_matching(CONFIGFS_FUNCTIONS, "acm.", true);
    remove_matching(CONFIGFS_FUNCTIONS, "ffs.", true);
    snprintf(path, sizeof(path), "%s/ffs", CONFIGFS_FUNCTIONS);
    unlink_if_exists(path);

    if (!write_text(PIK_CONFIGFS_GADGET "/idVendor", PIK1_USB_ID_VENDOR) ||
        !write_text(PIK_CONFIGFS_GADGET "/idProduct", PIK1_USB_ID_PRODUCT) ||
        !write_text(PIK_CONFIGFS_GADGET "/bcdUSB", PIK1_USB_BCD_USB) ||
        !write_text(PIK_CONFIGFS_GADGET "/bcdDevice", PIK1_USB_BCD_DEVICE)) {
        LOG("write gadget ids: %s", strerror(errno));
        return false;
    }

    snprintf(path, sizeof(path), "%s/strings/%s", PIK_CONFIGFS_GADGET, PIK1_USB_LANG);
    if (!mkdir_p(path)) return false;
    snprintf(path, sizeof(path), "%s/strings/%s/manufacturer", PIK_CONFIGFS_GADGET, PIK1_USB_LANG);
    if (!write_text(path, PIK1_USB_MANUFACTURER)) return false;
    snprintf(path, sizeof(path), "%s/strings/%s/product", PIK_CONFIGFS_GADGET, PIK1_USB_LANG);
    if (!write_text(path, PIK1_USB_PRODUCT)) return false;
    snprintf(path, sizeof(path), "%s/strings/%s/serialnumber", PIK_CONFIGFS_GADGET, PIK1_USB_LANG);
    if (!write_text(path, PIK1_USB_SERIAL)) return false;

    if (!mkdir_p(CONFIGFS_CONFIG) ||
        !write_text(CONFIGFS_CONFIG "/MaxPower", "250"))
        return false;
    snprintf(path, sizeof(path), "%s/strings/%s", CONFIGFS_CONFIG, PIK1_USB_LANG);
    if (!mkdir_p(path)) return false;
    snprintf(path, sizeof(path), "%s/strings/%s/configuration", CONFIGFS_CONFIG, PIK1_USB_LANG);
    if (!write_text(path, PIK1_USB_CONFIGURATION)) return false;

    snprintf(path, sizeof(path), "%s/%s", CONFIGFS_FUNCTIONS, ffs_function);
    if (!mkdir_p(path)) {
        LOG("mkdir %s: %s", path, strerror(errno));
        return false;
    }
    char link_path[256];
    snprintf(link_path, sizeof(link_path), "%s/%s", CONFIGFS_CONFIG, ffs_function);
    unlink_if_exists(link_path);
    if (symlink(path, link_path) < 0) {
        LOG("link %s -> %s: %s", link_path, path, strerror(errno));
        return false;
    }

    if (!mkdir_p(PIK1_FFS_MOUNT)) {
        LOG("mkdir %s: %s", PIK1_FFS_MOUNT, strerror(errno));
        return false;
    }
    LOG("mounting FunctionFS at %s", PIK1_FFS_MOUNT);
    if (mount(PIK1_FFS_NAME, PIK1_FFS_MOUNT, "functionfs", 0, NULL) < 0) {
        LOG("mount FunctionFS %s: %s", PIK1_FFS_NAME, strerror(errno));
        return false;
    }
    LOG("mounted FunctionFS instance %s", PIK1_FFS_NAME);
    return true;
}
