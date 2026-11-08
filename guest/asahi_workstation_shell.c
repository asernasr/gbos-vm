/*
 * Googlebook OS on Apple Silicon - Workstation Root Shell & Linux Environment Multiplexer
 *
 * Modes:
 *  1. Server mode (`workstation-shell --daemon`):
 *     Started by Android init as root. Listens on /dev/socket/asahi_workstation
 *     (0660 root:shell). Spawns an interactive PTY session as root either in the
 *     native Android environment or inside the persistent Linux workstation rootfs
 *     (/data/asahi-linux, providing pacman/apt/nix, clang, git, python, tmux, etc.).
 *  2. Client mode (`workstation-shell [command...]` or `workstation-shell --linux`):
 *     Connects to /dev/socket/asahi_workstation from any unprivileged terminal or
 *     shell session and bridges raw terminal I/O.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define WORKSTATION_SOCK "/dev/socket/asahi_workstation"
#define LINUX_ROOTFS     "/data/asahi-linux"

static void setup_linux_mounts(void) {
    mkdir(LINUX_ROOTFS, 0755);
    mkdir(LINUX_ROOTFS "/proc", 0755);
    mkdir(LINUX_ROOTFS "/sys", 0755);
    mkdir(LINUX_ROOTFS "/dev", 0755);
    mkdir(LINUX_ROOTFS "/dev/pts", 0755);
    mkdir(LINUX_ROOTFS "/tmp", 01777);
    mkdir(LINUX_ROOTFS "/root", 0700);
    mkdir(LINUX_ROOTFS "/host", 0755);

    struct stat st;
    if (stat(LINUX_ROOTFS "/proc/self", &st) != 0) {
        mount("proc", LINUX_ROOTFS "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    }
    if (stat(LINUX_ROOTFS "/sys/class", &st) != 0) {
        mount("sysfs", LINUX_ROOTFS "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    }
    if (stat(LINUX_ROOTFS "/dev/null", &st) != 0) {
        mount("/dev", LINUX_ROOTFS "/dev", NULL, MS_BIND | MS_REC, NULL);
    }
}

static void handle_client_session(int client_fd) {
    char hdr[256] = {0};
    ssize_t n = recv(client_fd, hdr, sizeof(hdr) - 1, 0);
    bool use_linux_chroot = false;
    char *custom_cmd = NULL;
    if (n > 0) {
        hdr[n] = '\0';
        if (strncmp(hdr, "LINUX:", 6) == 0) {
            use_linux_chroot = true;
            if (hdr[6] != '\0') custom_cmd = hdr + 6;
        } else if (strncmp(hdr, "HOST:", 5) == 0) {
            if (hdr[5] != '\0') custom_cmd = hdr + 5;
        }
    }

    int master_fd = -1;
    struct winsize ws = { .ws_row = 40, .ws_col = 120 };
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        close(client_fd);
        _exit(1);
    }

    if (pid == 0) {
        close(client_fd);
        clearenv();
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("LANG", "C.UTF-8", 1);

        struct stat st;
        if (use_linux_chroot && stat(LINUX_ROOTFS "/bin/sh", &st) == 0) {
            setup_linux_mounts();
            if (chroot(LINUX_ROOTFS) == 0 && chdir("/root") == 0) {
                setenv("HOME", "/root", 1);
                setenv("USER", "root", 1);
                setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
                if (custom_cmd && custom_cmd[0]) {
                    execl("/bin/sh", "sh", "-c", custom_cmd, (char *)NULL);
                }
                if (access("/bin/bash", X_OK) == 0) {
                    execl("/bin/bash", "-bash", "-i", (char *)NULL);
                }
                execl("/bin/sh", "-sh", "-i", (char *)NULL);
            }
        }

        mkdir("/data/local/root-home", 0700);
        chdir("/data/local/root-home");
        setenv("HOME", "/data/local/root-home", 1);
        setenv("USER", "root", 1);
        setenv("PATH",
               "/data/asahi-linux/usr/local/bin:/data/asahi-linux/usr/bin:"
               "/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:"
               "/system_ext/bin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin",
               1);
        if (custom_cmd && custom_cmd[0]) {
            execl("/system/bin/sh", "sh", "-c", custom_cmd, (char *)NULL);
        }
        execl("/system/bin/sh", "-sh", "-i", (char *)NULL);
        _exit(127);
    }

    uint8_t buf[4096];
    struct pollfd pfds[2] = {
        { .fd = client_fd, .events = POLLIN },
        { .fd = master_fd, .events = POLLIN },
    };

    while (1) {
        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[0].revents & POLLIN) {
            ssize_t r = read(client_fd, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(master_fd, buf, (size_t)r) <= 0) break;
        }
        if (pfds[1].revents & POLLIN) {
            ssize_t r = read(master_fd, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(client_fd, buf, (size_t)r) <= 0) break;
        }
        if ((pfds[0].revents | pfds[1].revents) & (POLLHUP | POLLERR | POLLNVAL)) {
            break;
        }
    }

    close(master_fd);
    close(client_fd);
    kill(pid, SIGHUP);
    waitpid(pid, NULL, 0);
    _exit(0);
}

static int run_daemon(void) {
    signal(SIGCHLD, SIG_IGN);
    mkdir("/dev/socket", 0755);
    unlink(WORKSTATION_SOCK);

    int srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, WORKSTATION_SOCK, sizeof(addr.sun_path) - 1);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv_fd);
        return 1;
    }
    /* Accessible to root (0) and shell (2000) */
    chown(WORKSTATION_SOCK, 0, 2000);
    chmod(WORKSTATION_SOCK, 0660);

    if (listen(srv_fd, 8) < 0) {
        perror("listen");
        close(srv_fd);
        return 1;
    }

    while (1) {
        int client_fd = accept4(srv_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            sleep(1);
            continue;
        }
        pid_t child = fork();
        if (child == 0) {
            close(srv_fd);
            handle_client_session(client_fd);
        }
        close(client_fd);
    }
}

static int run_client(int argc, char **argv) {
    bool use_linux = false;
    int arg_start = 1;
    if (argc > 1 && strcmp(argv[1], "--linux") == 0) {
        use_linux = true;
        arg_start = 2;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, WORKSTATION_SOCK, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect(" WORKSTATION_SOCK ")");
        close(fd);
        return 1;
    }

    char hdr[256];
    int offset = snprintf(hdr, sizeof(hdr), "%s", use_linux ? "LINUX:" : "HOST:");
    for (int i = arg_start; i < argc && offset < (int)sizeof(hdr) - 2; i++) {
        offset += snprintf(hdr + offset, sizeof(hdr) - (size_t)offset,
                           "%s%s", (i > arg_start) ? " " : "", argv[i]);
    }
    send(fd, hdr, (size_t)offset + 1, 0);

    struct termios orig_tio;
    bool is_tty = isatty(STDIN_FILENO);
    if (is_tty && tcgetattr(STDIN_FILENO, &orig_tio) == 0) {
        struct termios raw_tio = orig_tio;
        cfmakeraw(&raw_tio);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);
    }

    uint8_t buf[4096];
    struct pollfd pfds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = fd,           .events = POLLIN },
    };

    while (1) {
        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[0].revents & POLLIN) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(fd, buf, (size_t)r) <= 0) break;
        }
        if (pfds[1].revents & POLLIN) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(STDOUT_FILENO, buf, (size_t)r) <= 0) break;
        }
        if ((pfds[0].revents | pfds[1].revents) & (POLLHUP | POLLERR | POLLNVAL)) {
            break;
        }
    }

    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        return run_daemon();
    }
    return run_client(argc, argv);
}
