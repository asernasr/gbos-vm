/*
 * Asahi Linux Bare-Metal Pre-Init Container Partition Mapper (`pre_init_asahi`)
 *
 * Runs as PID 1 (/init) in the first-stage initramfs before chaining into Android's
 * real first-stage init (/init.real -> /system/bin/init).
 *
 * Key Tasks:
 *  1. Mounts /dev (devtmpfs), /proc, and /sys.
 *  2. Loads critical Apple Silicon, loop, dm-verity/dm-crypt, and EROFS kernel modules
 *     via finit_module(273).
 *  3. Scans /dev/nvme0n1p* for the Googlebook OS inner GPT container image
 *     (containing "super" and "userdata" partitions).
 *  4. Attaches the container partition to /dev/loop0 with LO_FLAGS_PARTSCAN so the
 *     Linux kernel's native GPT parser exposes /sys/block/loop0/loop0p* with
 *     PARTNAME= uevents (required by Android's FirstStageDeviceInitializer).
 *     Falls back to a 512B-sector dm-linear wrapper under loop0 if the physical
 *     4096B NVMe sector size prevents 512B loop partition scanning.
 *  5. Chains into /init.real (Android's first-stage init).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <linux/loop.h>
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

#ifndef LOOP_SET_BLOCK_SIZE
#define LOOP_SET_BLOCK_SIZE 0x4C09
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

struct kmod_entry {
    const char *path;
    const char *params;
};

static const struct kmod_entry kmodules[] = {
    /* Loop device with 63 partitions per device */
    { "/lib/modules/loop.ko", "max_part=63" },
    /* Storage & IOMMU (ordered by dependency) */
    { "/lib/modules/apple-dart.ko", "" },
    { "/lib/modules/apple-sart.ko", "" },
    { "/lib/modules/nvme-keyring.ko", "" },
    { "/lib/modules/nvme-auth.ko", "" },
    { "/lib/modules/nvme-core.ko", "" },
    { "/lib/modules/nvme-apple.ko", "" },
    /* Device Mapper Verity & Crypto (for AVB & userdata encryption) */
    { "/lib/modules/reed_solomon.ko", "" },
    { "/lib/modules/dm-verity.ko", "" },
    { "/lib/modules/dm-crypt.ko", "" },
    { "/lib/modules/dm-inlinecrypt.ko", "" },
    /* Filesystems & Swap (Stage 1 + Stage 2 Android core) */
    { "/lib/modules/erofs.ko", "" },
    { "/lib/modules/lz4hc_compress.ko", "" },
    { "/lib/modules/f2fs.ko", "" },
    { "/lib/modules/fat.ko", "" },
    { "/lib/modules/vfat.ko", "" },
    { "/lib/modules/fuse.ko", "" },
    { "/lib/modules/overlay.ko", "" },
    { "/lib/modules/zram.ko", "" },
    /* Display (apple-dcp) & AGX GPU (asahi) */
    { "/lib/modules/soundcore.ko", "" },
    { "/lib/modules/snd.ko", "" },
    { "/lib/modules/snd-timer.ko", "" },
    { "/lib/modules/snd-pcm.ko", "" },
    { "/lib/modules/snd-pcm-dmaengine.ko", "" },
    { "/lib/modules/drm_dma_helper.ko", "" },
    { "/lib/modules/typec.ko", "" },
    { "/lib/modules/appledrm.ko", "" },
    { "/lib/modules/asahi.ko", "" },
    /* Hardware, Keyboard, Trackpad, Power & USB-C */
    { "/lib/modules/macsmc.ko", "" },
    { "/lib/modules/gpio-macsmc.ko", "" },
    { "/lib/modules/macsmc-power.ko", "" },
    { "/lib/modules/macsmc-reboot.ko", "" },
    { "/lib/modules/macsmc-hwmon.ko", "" },
    { "/lib/modules/rtc-macsmc.ko", "" },
    { "/lib/modules/apple-sio.ko", "" },
    { "/lib/modules/spi-apple.ko", "" },
    { "/lib/modules/spi-hid-apple.ko", "" },
    { "/lib/modules/spi-hid-apple-of.ko", "" },
    { "/lib/modules/pinctrl-apple-gpio.ko", "" },
    { "/lib/modules/apple_wdt.ko", "" },
    { "/lib/modules/apple-dockchannel.ko", "" },
    { "/lib/modules/dockchannel-hid.ko", "" },
    { "/lib/modules/hid-apple.ko", "" },
    { "/lib/modules/apple-tunable.ko", "" },
    { "/lib/modules/phy-apple-atc.ko", "" },
    { "/lib/modules/ulpi.ko", "" },
    { "/lib/modules/udc-core.ko", "" },
    { "/lib/modules/dwc3.ko", "" },
    { "/lib/modules/dwc3-apple.ko", "" },
    { "/lib/modules/tps6598x-core.ko", "" },
    { "/lib/modules/tps6598x.ko", "" },
    { NULL, NULL }
};

static void load_early_modules(void) {
    klog_msg("Loading essential Apple Silicon, loop, dm-verity & EROFS modules...");
    for (int i = 0; kmodules[i].path != NULL; i++) {
        const char *mpath = kmodules[i].path;
        const char *params = kmodules[i].params ? kmodules[i].params : "";
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

        long ret = syscall(__NR_finit_module, fd, params, 0);
        if (ret != 0 && errno != EEXIST) {
            char buf[256];
            snprintf(buf, sizeof(buf), "finit_module %s failed: errno %d (%s)",
                     mpath, errno, strerror(errno));
            klog_msg(buf);
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "finit_module %s: %s",
                     mpath, (errno == EEXIST) ? "already loaded" : "ok");
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

static bool probe_inner_gpt(const char *dev_path, uint64_t *out_sector_size, uint64_t *out_bytes) {
    int fd = open(dev_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    uint64_t container_bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &container_bytes) < 0 || container_bytes < 64ULL * 1024 * 1024) {
        close(fd);
        return false;
    }

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

    close(fd);
    if (!has_super || !has_userdata) {
        return false;
    }

    *out_sector_size = gpt_sector_size;
    *out_bytes = container_bytes;

    char msg[256];
    snprintf(msg, sizeof(msg), "Found Googlebook OS inner GPT on %s (sector=%llu, size=%llu MB)",
             dev_path, (unsigned long long)gpt_sector_size,
             (unsigned long long)(container_bytes / (1024ULL * 1024ULL)));
    klog_msg(msg);
    return true;
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

static bool create_dm_linear_wrapper(const char *backing_dev, uint64_t bytes, char *out_node, size_t out_sz) {
    mkdir("/dev/mapper", 0755);
    int dm_fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    if (dm_fd < 0) {
        mknod("/dev/mapper/control", S_IFCHR | 0600, makedev(10, 236));
        dm_fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    }
    if (dm_fd < 0) return false;

    uint8_t buf[sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec) + 256];
    struct dm_ioctl *io = (struct dm_ioctl *)buf;
    const char *dm_name = "gbos_disk";

    init_dm_ioctl(io, dm_name);
    snprintf(io->uuid, sizeof(io->uuid), "GBOS-ASAHI-DISK");
    if (ioctl(dm_fd, DM_DEV_CREATE, io) < 0 && errno != EEXIST) {
        close(dm_fd);
        return false;
    }

    memset(buf, 0, sizeof(buf));
    init_dm_ioctl(io, dm_name);
    io->data_size = sizeof(buf);
    io->data_start = sizeof(struct dm_ioctl);
    io->target_count = 1;

    struct dm_target_spec *spec = (struct dm_target_spec *)(buf + sizeof(struct dm_ioctl));
    spec->sector_start = 0;
    spec->length = bytes / SECTOR_SIZE;
    strcpy(spec->target_type, "linear");

    char *params = (char *)(spec + 1);
    int param_len = snprintf(params, 200, "%s 0", backing_dev);
    size_t total_spec = sizeof(struct dm_target_spec) + (size_t)param_len + 1;
    total_spec = (total_spec + 7U) & ~7U;
    spec->next = (uint32_t)total_spec;

    if (ioctl(dm_fd, DM_TABLE_LOAD, io) < 0) {
        close(dm_fd);
        return false;
    }

    init_dm_ioctl(io, dm_name);
    if (ioctl(dm_fd, DM_DEV_SUSPEND, io) < 0) {
        close(dm_fd);
        return false;
    }

    uint32_t dev_maj = major(io->dev);
    uint32_t dev_min = minor(io->dev);
    close(dm_fd);

    mkdir("/dev/block", 0755);
    snprintf(out_node, out_sz, "/dev/block/dm-gbos-disk");
    unlink(out_node);
    mknod(out_node, S_IFBLK | 0600, makedev(dev_maj, dev_min));
    return true;
}

static bool try_loop_attach(int lfd, int dev_nr, const char *backing_dev, uint64_t sector_size) {
    int bfd = open(backing_dev, O_RDWR | O_CLOEXEC);
    if (bfd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open %s O_RDWR: %s", backing_dev, strerror(errno));
        klog_msg(msg);
        return false;
    }

    if (ioctl(lfd, LOOP_SET_FD, bfd) < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "LOOP_SET_FD(%s) failed: %s", backing_dev, strerror(errno));
        klog_msg(msg);
        close(bfd);
        return false;
    }
    close(bfd);

    if (ioctl(lfd, LOOP_SET_BLOCK_SIZE, (unsigned long)sector_size) < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "LOOP_SET_BLOCK_SIZE(%llu) on %s failed: %s",
                 (unsigned long long)sector_size, backing_dev, strerror(errno));
        klog_msg(msg);
    }

    struct loop_info64 li;
    memset(&li, 0, sizeof(li));
    li.lo_flags = LO_FLAGS_PARTSCAN;
    strncpy((char *)li.lo_file_name, backing_dev, LO_NAME_SIZE - 1);
    if (ioctl(lfd, LOOP_SET_STATUS64, &li) < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "LOOP_SET_STATUS64 failed: %s", strerror(errno));
        klog_msg(msg);
        ioctl(lfd, LOOP_CLR_FD, 0);
        return false;
    }

    ioctl(lfd, BLKRRPART, 0);

    /* Wait up to 2 seconds for kernel to create /sys/block/loopN/loopNp1 */
    char sys_part[128];
    snprintf(sys_part, sizeof(sys_part), "/sys/block/loop%d/loop%dp1/uevent", dev_nr, dev_nr);
    for (int i = 0; i < 40; i++) {
        if (access(sys_part, F_OK) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Kernel exposed inner GPT partitions via /dev/loop%dp* (backing=%s)",
                     dev_nr, backing_dev);
            klog_msg(msg);
            return true;
        }
        usleep(50000);
    }

    klog_msg("Loop attach succeeded but kernel did not emit loop0p1; detaching...");
    ioctl(lfd, LOOP_CLR_FD, 0);
    return false;
}

static bool attach_container_via_loop(const char *container_dev, uint64_t sector_size, uint64_t bytes) {
    int ctl_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl_fd < 0) {
        mknod("/dev/loop-control", S_IFCHR | 0600, makedev(10, 237));
        ctl_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    }

    int dev_nr = 0;
    if (ctl_fd >= 0) {
        int free_nr = ioctl(ctl_fd, LOOP_CTL_GET_FREE);
        if (free_nr >= 0) dev_nr = free_nr;
        close(ctl_fd);
    }

    char loop_dev[64];
    snprintf(loop_dev, sizeof(loop_dev), "/dev/loop%d", dev_nr);
    int lfd = open(loop_dev, O_RDWR | O_CLOEXEC);
    if (lfd < 0) {
        mknod(loop_dev, S_IFBLK | 0660, makedev(7, (unsigned int)dev_nr));
        lfd = open(loop_dev, O_RDWR | O_CLOEXEC);
    }
    if (lfd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ERROR: Could not open %s: %s", loop_dev, strerror(errno));
        klog_msg(msg);
        return false;
    }

    if (try_loop_attach(lfd, dev_nr, container_dev, sector_size)) {
        close(lfd);
        return true;
    }

    /* Fallback: wrap container_dev in a 512B-sector dm-linear device so loop0 sees 512B LBA */
    char dm_node[128];
    if (create_dm_linear_wrapper(container_dev, bytes, dm_node, sizeof(dm_node))) {
        klog_msg("Retrying loop attach on 512B-sector dm-linear wrapper...");
        if (try_loop_attach(lfd, dev_nr, dm_node, sector_size)) {
            close(lfd);
            return true;
        }
    }

    close(lfd);
    return false;
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

    load_early_modules();

    klog_msg("Scanning for Googlebook OS inner GPT container partition...");
    char found_dev[64] = {0};
    uint64_t gpt_ssz = 512ULL;
    uint64_t container_bytes = 0;

    for (int attempt = 0; attempt < 50; attempt++) {
        for (int p = 1; p <= 16; p++) {
            char cand[64];
            snprintf(cand, sizeof(cand), "/dev/nvme0n1p%d", p);
            if (probe_inner_gpt(cand, &gpt_ssz, &container_bytes)) {
                strncpy(found_dev, cand, sizeof(found_dev) - 1);
                break;
            }
        }
        if (found_dev[0] == '\0' && probe_inner_gpt("/dev/vda", &gpt_ssz, &container_bytes)) {
            strncpy(found_dev, "/dev/vda", sizeof(found_dev) - 1);
        }
        if (found_dev[0] != '\0') break;
        usleep(200000);
    }

    if (found_dev[0] != '\0') {
        if (!attach_container_via_loop(found_dev, gpt_ssz, container_bytes)) {
            klog_msg("ERROR: Failed to attach container via loop with LO_FLAGS_PARTSCAN!");
        }
    } else {
        klog_msg("ERROR: No Googlebook OS inner GPT container found on /dev/nvme0n1p*!");
    }

    if (map_only) {
        return 0;
    }

    klog_msg("Pre-init setup complete. Chaining PID 1 to /init.real...");

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
