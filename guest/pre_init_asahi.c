/*
 * Asahi Linux Bare-Metal Pre-Init Container Partition Mapper (`pre_init_asahi`)
 *
 * Runs as PID 1 (/init) in the first-stage initramfs before chaining into Android's
 * real first-stage init (/init.real -> /system/bin/init).
 *
 * Key Tasks:
 *  1. Mounts /dev (devtmpfs), /proc, and /sys.
 *  2. Loads critical Apple Silicon kernel modules (apple-dart, apple-sart, nvme-core,
 *     nvme-apple, erofs, macsmc, spi-hid-apple, etc.) via finit_module syscall.
 *  3. Scans /dev/nvme0n1p* for the Googlebook OS inner GPT container image
 *     (containing "super" and "userdata" partitions).
 *  4. Creates dm-linear block devices via /dev/mapper/control ioctls for every inner
 *     partition and populates /dev/block/by-name/<partname> and aliases.
 *  5. Forks a persistent /dev watcher daemon to re-create /dev/mapper/control and
 *     all /dev/block/by-name/* symlinks when Android's first_stage_init mounts a
 *     clean tmpfs over /dev.
 *  6. Chains into /init.real (Android's first-stage init).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __NR_finit_module
#define __NR_finit_module 273
#endif

#define GPT_HEADER_SIGNATURE 0x5452415020494645ULL /* "EFI PART" */
#define SECTOR_SIZE          512ULL

struct __attribute__((packed)) gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved1;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_entry_array_crc32;
};

struct __attribute__((packed)) gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name_utf16[36];
};

struct mapped_partition {
    char name[64];
    char dm_name[64];
    uint32_t dev_maj;
    uint32_t dev_min;
};

static struct mapped_partition g_parts[64];
static int g_part_count = 0;

static void klog_msg(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
        fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    }
    if (fd >= 0) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "<6>pre_init_asahi: %s\n", msg);
        if (n > 0) write(fd, buf, (size_t)n);
        close(fd);
    }
}

static const char *kmodules[] = {
    /* Storage & IOMMU (ordered by dependency) */
    "/lib/modules/apple-dart.ko",
    "/lib/modules/apple-sart.ko",
    "/lib/modules/nvme-keyring.ko",
    "/lib/modules/nvme-auth.ko",
    "/lib/modules/nvme-core.ko",
    "/lib/modules/nvme-apple.ko",
    /* Filesystems */
    "/lib/modules/erofs.ko",
    "/lib/modules/lz4hc_compress.ko",
    "/lib/modules/f2fs.ko",
    /* Hardware, Keyboard, Trackpad & Power */
    "/lib/modules/macsmc.ko",
    "/lib/modules/gpio-macsmc.ko",
    "/lib/modules/macsmc-power.ko",
    "/lib/modules/macsmc-reboot.ko",
    "/lib/modules/macsmc-hwmon.ko",
    "/lib/modules/rtc-macsmc.ko",
    "/lib/modules/apple-sio.ko",
    "/lib/modules/spi-apple.ko",
    "/lib/modules/spi-hid-apple.ko",
    "/lib/modules/spi-hid-apple-of.ko",
    "/lib/modules/pinctrl-apple-gpio.ko",
    "/lib/modules/apple_wdt.ko",
    "/lib/modules/apple-dockchannel.ko",
    "/lib/modules/dockchannel-hid.ko",
    NULL
};

static void load_early_modules(void) {
    klog_msg("Loading essential Apple Silicon & filesystem kernel modules...");
    for (int i = 0; kmodules[i] != NULL; i++) {
        const char *mpath = kmodules[i];
        int fd = open(mpath, O_RDONLY | O_CLOEXEC);
        if (fd < 0 && strncmp(mpath, "/lib/", 5) == 0) {
            fd = open(mpath + 4, O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Module file not found: %s", mpath);
            klog_msg(buf);
            continue;
        }

        long ret = syscall(__NR_finit_module, fd, "", 0);
        if (ret != 0 && errno != EEXIST) {
            char buf[256];
            snprintf(buf, sizeof(buf), "finit_module %s failed: errno %d (%s)",
                     mpath, errno, strerror(errno));
            klog_msg(buf);
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "finit_module %s: %s",
                     mpath, (errno == EEXIST) ? "already loaded" : "success");
            klog_msg(buf);
        }
        close(fd);
    }
}

static void utf16le_to_ascii(const uint16_t *in, size_t max_chars, char *out) {
    size_t i = 0;
    for (; i < max_chars; i++) {
        uint16_t c = in[i];
        if (c == 0) break;
        out[i] = (c < 0x80) ? (char)c : '_';
    }
    out[i] = '\0';
}

static void init_dm_ioctl(struct dm_ioctl *io, const char *name) {
    memset(io, 0, sizeof(*io));
    io->version[0] = DM_VERSION_MAJOR;
    io->version[1] = DM_VERSION_MINOR;
    io->version[2] = DM_VERSION_PATCHLEVEL;
    io->data_size = sizeof(*io);
    io->data_start = sizeof(*io);
    if (name) {
        strncpy(io->name, name, sizeof(io->name) - 1);
    }
}

static void repopulate_dev_nodes(void) {
    mkdir("/dev/mapper", 0755);
    mknod("/dev/mapper/control", S_IFCHR | 0600, makedev(10, 236));
    mkdir("/dev/block", 0755);
    mkdir("/dev/block/by-name", 0755);
    mkdir("/dev/block/bootdevice", 0755);
    mkdir("/dev/block/bootdevice/by-name", 0755);
    mkdir("/dev/block/mapper", 0755);

    for (int i = 0; i < g_part_count; i++) {
        const char *pname = g_parts[i].name;
        dev_t dev = makedev(g_parts[i].dev_maj, g_parts[i].dev_min);

        char dm_node[128];
        snprintf(dm_node, sizeof(dm_node), "/dev/block/dm-%s", g_parts[i].dm_name);
        unlink(dm_node);
        mknod(dm_node, S_IFBLK | 0600, dev);

        char link_path[128];
        snprintf(link_path, sizeof(link_path), "/dev/block/by-name/%s", pname);
        unlink(link_path);
        symlink(dm_node, link_path);

        snprintf(link_path, sizeof(link_path), "/dev/block/bootdevice/by-name/%s", pname);
        unlink(link_path);
        symlink(dm_node, link_path);

        /* Handle slot-suffixed partitions (e.g. boot_a -> boot, vbmeta_a -> vbmeta) */
        size_t len = strlen(pname);
        if (len > 2 && pname[len - 2] == '_' && (pname[len - 1] == 'a' || pname[len - 1] == 'b')) {
            char base[64];
            strncpy(base, pname, len - 2);
            base[len - 2] = '\0';

            snprintf(link_path, sizeof(link_path), "/dev/block/by-name/%s", base);
            unlink(link_path);
            symlink(dm_node, link_path);

            snprintf(link_path, sizeof(link_path), "/dev/block/bootdevice/by-name/%s", base);
            unlink(link_path);
            symlink(dm_node, link_path);
        }

        /* Mirror into /dev/block/mapper/ */
        snprintf(link_path, sizeof(link_path), "/dev/block/mapper/%s", pname);
        unlink(link_path);
        symlink(dm_node, link_path);
    }
}

static int create_dm_linear_part(int dm_fd, const char *container_dev,
                                 const char *part_name, uint64_t start_sector,
                                 uint64_t num_sectors) {
    uint8_t buf[sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec) + 256];
    struct dm_ioctl *io = (struct dm_ioctl *)buf;

    char dm_name[64];
    snprintf(dm_name, sizeof(dm_name), "gbos_%s", part_name);

    /* 1. DM_DEV_CREATE */
    init_dm_ioctl(io, dm_name);
    snprintf(io->uuid, sizeof(io->uuid), "GBOS-ASAHI-%s", part_name);
    if (ioctl(dm_fd, DM_DEV_CREATE, io) < 0 && errno != EEXIST) {
        return -1;
    }

    /* 2. DM_TABLE_LOAD */
    memset(buf, 0, sizeof(buf));
    init_dm_ioctl(io, dm_name);
    io->data_size = sizeof(buf);
    io->data_start = sizeof(struct dm_ioctl);
    io->target_count = 1;

    struct dm_target_spec *spec = (struct dm_target_spec *)(buf + sizeof(struct dm_ioctl));
    spec->sector_start = 0;
    spec->length = num_sectors;
    strcpy(spec->target_type, "linear");

    char *params = (char *)(spec + 1);
    int param_len = snprintf(params, 200, "%s %llu",
                             container_dev, (unsigned long long)start_sector);
    size_t total_spec = sizeof(struct dm_target_spec) + (size_t)param_len + 1;
    total_spec = (total_spec + 7U) & ~7U;
    spec->next = (uint32_t)total_spec;

    if (ioctl(dm_fd, DM_TABLE_LOAD, io) < 0) {
        return -1;
    }

    /* 3. DM_DEV_SUSPEND (resume) */
    init_dm_ioctl(io, dm_name);
    if (ioctl(dm_fd, DM_DEV_SUSPEND, io) < 0) {
        return -1;
    }

    uint32_t dev_maj = major(io->dev);
    uint32_t dev_min = minor(io->dev);

    if (g_part_count < 64) {
        strncpy(g_parts[g_part_count].name, part_name, sizeof(g_parts[g_part_count].name) - 1);
        strncpy(g_parts[g_part_count].dm_name, dm_name, sizeof(g_parts[g_part_count].dm_name) - 1);
        g_parts[g_part_count].dev_maj = dev_maj;
        g_parts[g_part_count].dev_min = dev_min;
        g_part_count++;
    }

    return 0;
}

static bool probe_and_map_container(int dm_fd, const char *dev_path) {
    int fd = open(dev_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    uint64_t container_bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &container_bytes) < 0 || container_bytes < 64ULL * 1024 * 1024) {
        close(fd);
        return false;
    }

    /* Check both 512B LBA 1 (offset 512) and 4096B LBA 1 (offset 4096) for inner GPT */
    uint64_t gpt_sector_size = 512ULL;
    struct gpt_header hdr;
    bool found_gpt = false;

    const uint64_t candidate_sector_sizes[2] = { 512ULL, 4096ULL };
    for (int s = 0; s < 2; s++) {
        uint64_t ssz = candidate_sector_sizes[s];
        if (pread(fd, &hdr, sizeof(hdr), (off_t)ssz) == sizeof(hdr) &&
            hdr.signature == GPT_HEADER_SIGNATURE &&
            hdr.num_partition_entries > 0 &&
            hdr.num_partition_entries <= 128 &&
            hdr.sizeof_partition_entry >= sizeof(struct gpt_entry)) {
            gpt_sector_size = ssz;
            found_gpt = true;
            break;
        }
    }

    if (!found_gpt) {
        close(fd);
        return false;
    }

    /* Verify that this inner GPT contains "super" and "userdata" */
    bool has_super = false, has_userdata = false;
    off_t entries_off = (off_t)(hdr.partition_entry_lba * gpt_sector_size);
    for (uint32_t i = 0; i < hdr.num_partition_entries; i++) {
        struct gpt_entry ent;
        if (pread(fd, &ent, sizeof(ent),
                  entries_off + (off_t)i * hdr.sizeof_partition_entry) != sizeof(ent)) {
            break;
        }
        if (ent.starting_lba == 0) continue;
        char pname[64];
        utf16le_to_ascii(ent.name_utf16, 36, pname);
        if (strcmp(pname, "super") == 0) has_super = true;
        if (strcmp(pname, "userdata") == 0) has_userdata = true;
    }

    if (!has_super || !has_userdata) {
        close(fd);
        return false;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Found Googlebook OS inner GPT on %s (sector=%llu, entries=%u)",
             dev_path, (unsigned long long)gpt_sector_size, hdr.num_partition_entries);
    klog_msg(msg);

    uint64_t scale_to_512 = gpt_sector_size / SECTOR_SIZE;
    uint64_t container_512_sectors = container_bytes / SECTOR_SIZE;
    int mapped = 0;

    for (uint32_t i = 0; i < hdr.num_partition_entries; i++) {
        struct gpt_entry ent;
        if (pread(fd, &ent, sizeof(ent),
                  entries_off + (off_t)i * hdr.sizeof_partition_entry) != sizeof(ent)) {
            break;
        }
        if (ent.starting_lba == 0 || ent.ending_lba < ent.starting_lba) continue;
        char pname[64];
        utf16le_to_ascii(ent.name_utf16, 36, pname);
        if (pname[0] == '\0') continue;

        uint64_t start_512 = ent.starting_lba * scale_to_512;
        uint64_t count_512 = (ent.ending_lba - ent.starting_lba + 1ULL) * scale_to_512;

        /* If userdata is the final partition and the outer Asahi partition is larger,
         * allow userdata to span up to the end of the outer partition (minus 64 KiB backup). */
        if (strcmp(pname, "userdata") == 0 &&
            start_512 + count_512 < container_512_sectors - 128ULL) {
            count_512 = (container_512_sectors - 128ULL) - start_512;
        }

        if (create_dm_linear_part(dm_fd, dev_path, pname, start_512, count_512) == 0) {
            mapped++;
        }
    }

    close(fd);
    repopulate_dev_nodes();
    snprintf(msg, sizeof(msg), "Mapped %d inner Android partitions from %s via dm-linear",
             mapped, dev_path);
    klog_msg(msg);
    return (mapped > 0);
}

static void run_dev_watcher(void) {
    klog_msg("dev_watcher: background daemon active, protecting /dev during Android first_stage_mount");
    int loops = 0;
    while (1) {
        struct stat st;
        bool missing = false;
        if (stat("/dev/block/by-name/super", &st) != 0) {
            missing = true;
        }
        if (stat("/dev/mapper/control", &st) != 0) {
            missing = true;
        }

        if (missing) {
            repopulate_dev_nodes();
            klog_msg("dev_watcher: tmpfs overmount detected! Restored /dev/mapper/control and /dev/block/by-name/*");
        }

        loops++;
        if (loops < 2000) {
            usleep(5000);  /* 5ms during initial 10s bringup */
        } else {
            usleep(100000); /* 100ms steady state */
        }
    }
}

int main(int argc, char **argv, char **envp) {
    bool is_pid1 = (getpid() == 1);
    bool map_only = (argc > 1 && strcmp(argv[1], "--map-only") == 0);
    if (!is_pid1 && !map_only) {
        return 0;
    }

    mkdir("/dev", 0755);
    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    klog_msg("=== Googlebook OS Bare-Metal Apple Silicon Pre-Init Starting ===");

    /* Load early drivers so Apple ANS2 NVMe controller and filesystems are probed */
    load_early_modules();

    mkdir("/dev/mapper", 0755);
    int dm_fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    if (dm_fd < 0) {
        mknod("/dev/mapper/control", S_IFCHR | 0600, makedev(10, 236));
        dm_fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    }

    if (dm_fd >= 0) {
        /* Wait up to 10 seconds for Apple ANS2 NVMe (/dev/nvme0n1p*) to enumerate */
        klog_msg("Scanning for Googlebook OS inner GPT container partition...");
        for (int attempt = 0; attempt < 50; attempt++) {
            bool done = false;
            for (int p = 1; p <= 16; p++) {
                char cand[64];
                snprintf(cand, sizeof(cand), "/dev/nvme0n1p%d", p);
                if (probe_and_map_container(dm_fd, cand)) {
                    done = true;
                    break;
                }
            }
            if (!done && probe_and_map_container(dm_fd, "/dev/vda")) {
                done = true;
            }
            if (done) break;
            usleep(200000);
        }
        close(dm_fd);
    } else {
        klog_msg("ERROR: Failed to open /dev/mapper/control!");
    }

    if (map_only) {
        return 0;
    }

    /* Spawn background daemon to watch and repopulate /dev when Android overmounts tmpfs */
    pid_t wpid = fork();
    if (wpid == 0) {
        run_dev_watcher();
        _exit(0);
    } else if (wpid > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Spawned dev_watcher daemon (PID %d)", (int)wpid);
        klog_msg(buf);
    }

    klog_msg("Pre-init container mapping complete. Chaining PID 1 to /init.real...");

    /* Hand off PID 1 to Android's real first-stage init */
    if (access("/init.real", X_OK) == 0) {
        execve("/init.real", argv, envp);
    }
    execve("/system/bin/init", argv, envp);

    char err[128];
    snprintf(err, sizeof(err), "FATAL: execve failed: %s", strerror(errno));
    klog_msg(err);

    while (1) sleep(1);
    return 1;
}
