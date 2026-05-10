#include "usb_discovery.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYSPATH_MAX 512
#define MAX_USB_DEVS 16

static bool sysfs_read(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return true;
}

static bool find_usb_dir(const char *sysfs_dev_path, char *out_dir, size_t out_cap) {
    char cur[SYSPATH_MAX];
    if (!realpath(sysfs_dev_path, cur)) return false;
    for (int i = 0; i < 8; i++) {
        char test[SYSPATH_MAX + 16];
        snprintf(test, sizeof(test), "%s/idVendor", cur);
        if (access(test, R_OK) == 0) {
            snprintf(out_dir, out_cap, "%s", cur);
            return true;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = '\0';
    }
    return false;
}

static void to_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'F') *s += 32;
}

static const char *strip_0x(char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    to_lower(s);
    return s;
}

static bool configfs_gadget_matches(const char *want_vid, const char *want_pid) {
    DIR *d = opendir("/sys/kernel/config/usb_gadget");
    if (!d) return false;
    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[SYSPATH_MAX + 32], vid[16], pid[16];
        snprintf(path, sizeof(path),
                 "/sys/kernel/config/usb_gadget/%s/idVendor", ent->d_name);
        if (!sysfs_read(path, vid, sizeof(vid))) continue;
        snprintf(path, sizeof(path),
                 "/sys/kernel/config/usb_gadget/%s/idProduct", ent->d_name);
        if (!sysfs_read(path, pid, sizeof(pid))) continue;
        found = strcmp(strip_0x(vid), want_vid) == 0 &&
                strcmp(strip_0x(pid), want_pid) == 0;
    }
    closedir(d);
    return found;
}

static int usb_dev_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

const char *usb_find_serial_dev(const char *vidpid, int n) {
    char want_vid[8], want_pid[8];
    if (sscanf(vidpid, "%7[^:]:%7s", want_vid, want_pid) != 2) return NULL;
    to_lower(want_vid);
    to_lower(want_pid);

    static const char *prefixes[] = { "ttyACM", "ttyGS", NULL };
    static char names[MAX_USB_DEVS][256];
    static const char *ptrs[MAX_USB_DEVS];
    int count = 0;

    DIR *d = opendir("/sys/class/tty");
    if (!d) return NULL;

    struct dirent *ent;
    while (count < MAX_USB_DEVS && (ent = readdir(d)) != NULL) {
        bool match = false;
        for (int i = 0; prefixes[i]; i++) {
            if (strncmp(ent->d_name, prefixes[i], strlen(prefixes[i])) == 0) {
                match = true;
                break;
            }
        }
        if (!match) continue;

        if (strncmp(ent->d_name, "ttyGS", 5) == 0) {
            if (configfs_gadget_matches(want_vid, want_pid)) {
                snprintf(names[count], sizeof(names[count]), "%s", ent->d_name);
                ptrs[count] = names[count];
                count++;
            }
            continue;
        }

        char devlink[SYSPATH_MAX];
        snprintf(devlink, sizeof(devlink), "/sys/class/tty/%s/device", ent->d_name);
        char usb_dir[SYSPATH_MAX];
        if (!find_usb_dir(devlink, usb_dir, sizeof(usb_dir))) continue;

        char vid[8], pid[8], tmp[SYSPATH_MAX + 16];
        snprintf(tmp, sizeof(tmp), "%s/idVendor", usb_dir);
        if (!sysfs_read(tmp, vid, sizeof(vid))) continue;
        snprintf(tmp, sizeof(tmp), "%s/idProduct", usb_dir);
        if (!sysfs_read(tmp, pid, sizeof(pid))) continue;
        to_lower(vid);
        to_lower(pid);

        if (strcmp(vid, want_vid) == 0 && strcmp(pid, want_pid) == 0) {
            snprintf(names[count], sizeof(names[count]), "%s", ent->d_name);
            ptrs[count] = names[count];
            count++;
        }
    }
    closedir(d);

    if (count == 0 || n >= count) return NULL;
    qsort(ptrs, (size_t)count, sizeof(ptrs[0]), usb_dev_cmp);

    static char result[64];
    snprintf(result, sizeof(result), "/dev/%s", ptrs[n]);
    return result;
}
