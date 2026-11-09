/* Add the three narrow rules graphics clients need to share memfd-backed buffers
 * (each one matches a denial seen in the guest log), plus optional bare-metal
 * Apple Silicon rules (--asahi-baremetal) for DRM/DCP, BCM4377 Bluetooth PTY,
 * and workstation shell socket access.
 * Does not change permissive domains, policy capabilities, MLS or constraints.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sepol/policydb/policydb.h>
#include <sepol/policydb/hashtab.h>
#include <sepol/policydb/avtab.h>

static int add_allow_rule(policydb_t *p, const char *src_name, const char *tgt_name,
                          const char *cls_name, const char *const *perms, unsigned n_perms) {
    type_datum_t *source = hashtab_search(p->p_types.table, (hashtab_key_t)src_name);
    type_datum_t *target = hashtab_search(p->p_types.table, (hashtab_key_t)tgt_name);
    class_datum_t *c = hashtab_search(p->p_classes.table, (hashtab_key_t)cls_name);
    if (!source || !target || !c) return -1;

    unsigned bits = 0;
    for (unsigned i = 0; i < n_perms; i++) {
        perm_datum_t *v = hashtab_search(c->permissions.table, (hashtab_key_t)perms[i]);
        if (!v && c->comdatum) {
            v = hashtab_search(c->comdatum->permissions.table, (hashtab_key_t)perms[i]);
        }
        if (!v || !v->s.value || v->s.value > 32) continue;
        bits |= 1U << (v->s.value - 1);
    }
    if (!bits) return -1;

    avtab_key_t key = {
        .source_type = source->s.value,
        .target_type = target->s.value,
        .target_class = c->s.value,
        .specified = AVTAB_ALLOWED,
    };
    avtab_datum_t *old = avtab_search(&p->te_avtab, &key);
    if (old) {
        old->data |= bits;
    } else {
        avtab_datum_t val = { .data = bits };
        if (avtab_insert(&p->te_avtab, &key, &val)) return -2;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) return 2;
    int asahi_bm = (argc == 4 && strcmp(argv[3], "--asahi-baremetal") == 0);
    policydb_t p;
    policy_file_t f;
    policydb_init(&p);
    policy_file_init(&f);
    f.type = PF_USE_STDIO;
    f.fp = fopen(argv[1], "rb");
    if (!f.fp) return 3;
    if (policydb_read(&p, &f, 0)) return 4;
    fclose(f.fp);

    type_datum_t *target = hashtab_search(p.p_types.table, "hal_graphics_allocator_default");
    class_datum_t *c = hashtab_search(p.p_classes.table, "memfd_file");
    if (!target || !c) return 5;
    const char *perms[] = {"read", "write", "map", "getattr"};
    unsigned bits = 0;
    for (unsigned i = 0; i < 4; i++) {
        perm_datum_t *v = hashtab_search(c->permissions.table, (hashtab_key_t)perms[i]);
        if (!v && c->comdatum) v = hashtab_search(c->comdatum->permissions.table, (hashtab_key_t)perms[i]);
        if (!v || !v->s.value || v->s.value > 32) return 6;
        bits |= 1U << (v->s.value - 1);
    }
    const char *sources[] = {"platform_app", "priv_app", "priv_app_36"};
    for (unsigned i = 0; i < 3; i++) {
        type_datum_t *source = hashtab_search(p.p_types.table, (hashtab_key_t)sources[i]);
        if (!source) return 7;
        avtab_key_t key = {
            .source_type = source->s.value,
            .target_type = target->s.value,
            .target_class = c->s.value,
            .specified = AVTAB_ALLOWED,
        };
        avtab_datum_t *old = avtab_search(&p.te_avtab, &key);
        printf("allow %s hal_graphics_allocator_default:memfd_file { read write map getattr }; prior_bits=%08x added_bits=%08x\n",
               sources[i], old ? old->data : 0, bits & ~(old ? old->data : 0));
        if (old) {
            old->data |= bits;
        } else {
            avtab_datum_t val = { .data = bits };
            if (avtab_insert(&p.te_avtab, &key, &val)) return 8;
        }
    }

    if (asahi_bm) {
        const char *chr_rw[] = {"read", "write", "open", "getattr", "ioctl", "map"};
        const char *sys_ro[] = {"read", "open", "getattr"};
        const char *dir_ro[] = {"read", "open", "getattr", "search"};
        const char *lnk_ro[] = {"read", "getattr"};
        const char *sock_rw[] = {"read", "write", "getattr", "connectto"};
        const char *sf_clients[] = {
            "hal_graphics_allocator_default",
            "hal_graphics_composer_default",
            "surfaceflinger",
            "system_server",
        };
        for (unsigned i = 0; i < 4; i++) {
            add_allow_rule(&p, sf_clients[i], "gpu_device", "chr_file", chr_rw, 6);
            add_allow_rule(&p, sf_clients[i], "sysfs_gpu", "file", sys_ro, 3);
            add_allow_rule(&p, sf_clients[i], "sysfs_gpu", "dir", dir_ro, 4);
            add_allow_rule(&p, sf_clients[i], "sysfs_gpu", "lnk_file", lnk_ro, 2);
        }
        /* Allow Bluetooth HAL to use the BCM4377 HCI-to-PTY symlink and devpts slave */
        add_allow_rule(&p, "hal_bluetooth_default", "hci_attach_dev", "lnk_file", lnk_ro, 2);
        add_allow_rule(&p, "hal_bluetooth_default", "hci_attach_dev", "chr_file", chr_rw, 5);
        add_allow_rule(&p, "hal_bluetooth_default", "devpts", "chr_file", chr_rw, 5);
        /* Allow Lights HAL & Health HAL to access Apple Silicon backlight (apple-panel-bl) and SMC battery */
        const char *sys_rw[] = {"read", "write", "open", "getattr"};
        const char *light_targets[] = {"sysfs", "sysfs_leds", "sysfs_gpu", "vendor_sysfs_graphics"};
        for (unsigned i = 0; i < 4; i++) {
            add_allow_rule(&p, "hal_light_default", light_targets[i], "file", sys_rw, 4);
            add_allow_rule(&p, "hal_light_default", light_targets[i], "dir", dir_ro, 4);
            add_allow_rule(&p, "hal_light_default", light_targets[i], "lnk_file", lnk_ro, 2);
            add_allow_rule(&p, "hal_health_default", light_targets[i], "file", sys_ro, 3);
            add_allow_rule(&p, "hal_health_default", light_targets[i], "dir", dir_ro, 4);
            add_allow_rule(&p, "hal_health_default", light_targets[i], "lnk_file", lnk_ro, 2);
            add_allow_rule(&p, "hal_camera_default", light_targets[i], "file", sys_ro, 3);
            add_allow_rule(&p, "hal_camera_default", light_targets[i], "dir", dir_ro, 4);
            add_allow_rule(&p, "hal_camera_default", light_targets[i], "lnk_file", lnk_ro, 2);
        }
        /* Allow Camera HAL & cameraserver to open V4L2 apple-isp (/dev/video*, /dev/media*) */
        add_allow_rule(&p, "hal_camera_default", "video_device", "chr_file", chr_rw, 6);
        add_allow_rule(&p, "hal_camera_default", "device", "dir", dir_ro, 4);
        add_allow_rule(&p, "cameraserver", "video_device", "chr_file", chr_rw, 6);
        /* Allow shell / terminal apps to connect to the workstation root PTY daemon */
        add_allow_rule(&p, "shell", "socket_device", "sock_file", chr_rw, 4);
        add_allow_rule(&p, "shell", "init", "unix_stream_socket", sock_rw, 4);
        printf("Added bare-metal Apple Silicon SELinux rules (gpu_device, sysfs_gpu, lights, health, camera, devpts, workstation).\n");
    }

    f.fp = fopen(argv[2], "wb");
    if (!f.fp) return 9;
    if (policydb_write(&p, &f) || fclose(f.fp)) return 10;
    policydb_destroy(&p);
    return 0;
}
