/*
 * Apple Silicon (MacBookAir10,1 / t8103 / j313) Bare-Metal Hardware Bridge Daemon
 *
 * Bridges Apple Silicon kernel hardware subsystems to Android's userspace HALs:
 *  1. Bluetooth (hci_bcm4377): Opens AF_BLUETOOTH HCI_CHANNEL_USER on hci0 and
 *     bridges H4 HCI packets to a pseudo-terminal symlinked at /dev/asahi_bt_hci
 *     for android.hardware.bluetooth-service.cuttlefish.
 *  2. Display & Keyboard Backlight: Synchronizes Android brightness properties with
 *     /sys/class/backlight/apple-panel-bl/brightness and /sys/class/leds/kbd_backlight.
 *  3. Apple SMC Battery & AC Power Supply: Reads /sys/class/power_supply/macsmc-battery
 *     and /sys/class/power_supply/macsmc-ac and updates Android battery telemetry.
 *  4. Lid Switch (s2idle) & Headphone Jack Sense (CS42L83): Monitors /dev/input/event*
 *     for SW_LID and SW_HEADPHONE_INSERT, notifying asahi-speakersafetyd and power management.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <android/log.h>
#include <sys/system_properties.h>

#define LOG_TAG "AsahiHwDaemon"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Linux Bluetooth HCI raw user channel definitions */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH        31
#endif
#define BTPROTO_HCI         1
#define HCI_CHANNEL_USER    1
#define HCIDEVDOWN          _IOW('H', 202, int)

struct sockaddr_hci {
    sa_family_t    hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

#define BT_PTY_SYMLINK "/dev/asahi_bt_hci"

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static long read_sysfs_long(const char *path, long fallback) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fallback;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return fallback;
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

static int read_sysfs_str(const char *path, char *out, size_t out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, out_len - 1);
    close(fd);
    if (n <= 0) return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
        n--;
    }
    out[n] = '\0';
    return 0;
}

static int write_sysfs_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return (n > 0) ? 0 : -1;
}

/*
 * Thread 1: Broadcom BCM4377 (hci0) HCI_CHANNEL_USER <-> /dev/asahi_bt_hci PTY bridge
 */
static void *bt_hci_bridge_thread(void *arg) {
    (void)arg;
    int master_fd = -1, slave_fd = -1;
    char slave_name[128] = {0};

    if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
        ALOGE("openpty failed for Bluetooth HCI bridge: %s", strerror(errno));
        return NULL;
    }
    struct termios tio;
    if (tcgetattr(master_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(master_fd, TCSANOW, &tio);
    }
    if (tcgetattr(slave_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(slave_fd, TCSANOW, &tio);
    }
    chmod(slave_name, 0660);
    chown(slave_name, 1002 /* AID_BLUETOOTH */, 1002 /* AID_BLUETOOTH */);
    unlink(BT_PTY_SYMLINK);
    symlink(slave_name, BT_PTY_SYMLINK);
    ALOGI("Created Bluetooth HCI PTY bridge at %s -> %s", BT_PTY_SYMLINK, slave_name);

    int hci_fd = -1;
    uint8_t buf[4096];

    while (g_running) {
        if (hci_fd < 0) {
            int ctl_sock = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
            if (ctl_sock >= 0) {
                /* HCI_CHANNEL_USER requires hci0 to be down before binding */
                ioctl(ctl_sock, HCIDEVDOWN, 0);
                close(ctl_sock);
            }
            hci_fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
            if (hci_fd >= 0) {
                struct sockaddr_hci addr = {
                    .hci_family = AF_BLUETOOTH,
                    .hci_dev = 0,
                    .hci_channel = HCI_CHANNEL_USER,
                };
                if (bind(hci_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                    close(hci_fd);
                    hci_fd = -1;
                } else {
                    ALOGI("Bound AF_BLUETOOTH HCI_CHANNEL_USER to hci0 (bcm4377)");
                    __system_property_set("vendor.asahi.bt.ready", "1");
                }
            }
            if (hci_fd < 0) {
                sleep(2);
                continue;
            }
        }

        struct pollfd pfds[2] = {
            { .fd = master_fd, .events = POLLIN },
            { .fd = hci_fd,    .events = POLLIN },
        };
        int pr = poll(pfds, 2, 1000);
        if (pr <= 0) continue;

        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                send(hci_fd, buf, (size_t)n, 0);
            }
        }
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(hci_fd);
            hci_fd = -1;
            continue;
        }
        if (pfds[1].revents & POLLIN) {
            ssize_t n = recv(hci_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                write(master_fd, buf, (size_t)n);
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                close(hci_fd);
                hci_fd = -1;
            }
        }
    }

    if (hci_fd >= 0) close(hci_fd);
    if (slave_fd >= 0) close(slave_fd);
    if (master_fd >= 0) close(master_fd);
    return NULL;
}

/*
 * Thread 2: Input event monitor for SW_LID (s2idle sleep/wake) and SW_HEADPHONE_INSERT (CS42L83)
 */
static void *input_switch_monitor_thread(void *arg) {
    (void)arg;
    int event_fds[16];
    int num_fds = 0;

    DIR *dir = opendir("/dev/input");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL && num_fds < 16) {
            if (strncmp(de->d_name, "event", 5) == 0) {
                char path[128];
                snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
                int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) {
                    unsigned long swbit[1] = {0};
                    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(swbit)), swbit) >= 0 && swbit[0] != 0) {
                        /* Query initial switch state */
                        unsigned long swstate[1] = {0};
                        if (ioctl(fd, EVIOCGSW(sizeof(swstate)), swstate) >= 0) {
                            int hp = (swstate[0] & (1UL << SW_HEADPHONE_INSERT)) ? 1 : 0;
                            __system_property_set("vendor.asahi.audio.headphones", hp ? "1" : "0");
                        }
                        event_fds[num_fds++] = fd;
                    } else {
                        close(fd);
                    }
                }
            }
        }
        closedir(dir);
    }

    struct pollfd pfds[16];
    for (int i = 0; i < num_fds; i++) {
        pfds[i].fd = event_fds[i];
        pfds[i].events = POLLIN;
    }

    while (g_running) {
        if (num_fds == 0) {
            sleep(5);
            continue;
        }
        int pr = poll(pfds, (nfds_t)num_fds, 2000);
        if (pr <= 0) continue;

        for (int i = 0; i < num_fds; i++) {
            if (pfds[i].revents & POLLIN) {
                struct input_event ev;
                while (read(pfds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                    if (ev.type == EV_SW) {
                        if (ev.code == SW_HEADPHONE_INSERT) {
                            ALOGI("CS42L83 Headphone Jack state changed: %d", ev.value);
                            __system_property_set("vendor.asahi.audio.headphones",
                                                  ev.value ? "1" : "0");
                        } else if (ev.code == SW_LID) {
                            ALOGI("Apple SMC Lid Switch state changed: %s",
                                  ev.value ? "CLOSED" : "OPEN");
                            __system_property_set("vendor.asahi.lid.closed",
                                                  ev.value ? "1" : "0");
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < num_fds; i++) {
        close(event_fds[i]);
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    ALOGI("Starting Apple Silicon (MacBookAir10,1 / t8103) Hardware Bridge Daemon");

    pthread_t bt_tid, sw_tid;
    pthread_create(&bt_tid, NULL, bt_hci_bridge_thread, NULL);
    pthread_create(&sw_tid, NULL, input_switch_monitor_thread, NULL);

    const char *bl_dir = "/sys/class/backlight/apple-panel-bl";
    long max_bl = read_sysfs_long("/sys/class/backlight/apple-panel-bl/max_brightness", 500);
    if (max_bl <= 0) max_bl = 500;
    long last_android_bl = -1;

    while (g_running) {
        /* 1. Sync display brightness (/sys/class/backlight/apple-panel-bl/brightness) */
        char bl_prop[32] = {0};
        if (__system_property_get("vendor.asahi.display.brightness", bl_prop) > 0 && bl_prop[0]) {
            long val = strtol(bl_prop, NULL, 10);
            if (val >= 0 && val <= 255 && val != last_android_bl) {
                long hw_val = (val * max_bl) / 255;
                if (val > 0 && hw_val == 0) hw_val = 1;
                char hw_str[32];
                snprintf(hw_str, sizeof(hw_str), "%ld", hw_val);
                write_sysfs_str("/sys/class/backlight/apple-panel-bl/brightness", hw_str);
                last_android_bl = val;
            }
        }
        (void)bl_dir;

        /* 2. Sync keyboard backlight (/sys/class/leds/kbd_backlight/brightness) */
        char kbd_prop[32] = {0};
        if (__system_property_get("vendor.asahi.kbd.brightness", kbd_prop) > 0 && kbd_prop[0]) {
            write_sysfs_str("/sys/class/leds/kbd_backlight/brightness", kbd_prop);
        }

        /* 3. Poll Apple SMC battery & AC adapter (/sys/class/power_supply/macsmc-battery) */
        long cap = read_sysfs_long("/sys/class/power_supply/macsmc-battery/capacity", -1);
        long ac  = read_sysfs_long("/sys/class/power_supply/macsmc-ac/online", -1);
        long temp_deci = read_sysfs_long("/sys/class/power_supply/macsmc-battery/temp", 250);
        char status_str[32] = {0};
        if (cap >= 0) {
            read_sysfs_str("/sys/class/power_supply/macsmc-battery/status",
                           status_str, sizeof(status_str));
            char prop_buf[32];
            snprintf(prop_buf, sizeof(prop_buf), "%ld", cap);
            __system_property_set("vendor.asahi.battery.capacity", prop_buf);
            snprintf(prop_buf, sizeof(prop_buf), "%ld", ac > 0 ? 1L : 0L);
            __system_property_set("vendor.asahi.battery.ac_online", prop_buf);
            snprintf(prop_buf, sizeof(prop_buf), "%ld", temp_deci);
            __system_property_set("vendor.asahi.battery.temp_deci", prop_buf);
            if (status_str[0]) {
                __system_property_set("vendor.asahi.battery.status", status_str);
            }
        }

        usleep(500000);
    }

    pthread_join(bt_tid, NULL);
    pthread_join(sw_tid, NULL);
    return 0;
}
