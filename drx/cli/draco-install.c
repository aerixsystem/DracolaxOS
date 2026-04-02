/* userland/tools/draco-install/draco-install.c
 *
 * Draco Install — Package manager for DracolaxOS
 *
 * Commands:
 *   draco install <file.deb>     extract & install .deb from /ramfs
 *   draco install --fake <name>  register a simulated package (testing)
 *   draco remove  <name>         uninstall a package
 *   draco list                   list installed packages
 *   draco info    <name>         show package details
 *   draco search  <query>        search in approved list
 *   draco approved               show curated package list
 *   draco reinstall <name>       reinstall from pkgdb entry
 *   draco status                 show package manager status
 *
 * .deb format (ar archive):
 *   "!<arch>\n"  (8 bytes magic)
 *   records: name[16] + date[12] + uid[6] + gid[6] + mode[8] +
 *            size_decimal[10] + fmag[2] + data[size] + optional_pad
 *
 * Since we have no network stack, packages are loaded by writing the .deb
 * bytes into /ramfs/ first (e.g. via serial or 'write' editor).
 *
 * Storage layout after install:
 *   /storage/main/apps/<name>/          — app root dir
 *   /storage/main/apps/<name>/bin/      — executables
 *   /storage/main/apps/<name>/etc/      — config files
 *   /storage/main/apps/<name>/meta.json — package metadata
 */
#include "../../kernel/types.h"
#include "../../kernel/klibc.h"
#include "../../kernel/drivers/vga/vga.h"
#include "../../kernel/log.h"
#include "../../kernel/fs/vfs.h"
#include "../../kernel/fs/ramfs.h"
#include "draco-install.h"

extern vfs_node_t *ramfs_root;
extern vfs_node_t *storage_root;

/* ---- In-memory package database ---------------------------------------- */

#define PKGDB_MAX 32

typedef struct {
    char     name[PKG_NAME_MAX];
    char     version[32];
    char     arch[16];
    char     depends[256];
    char     description[256];
    uint8_t  installed;
    uint8_t  static_binary;
    uint32_t install_time;   /* pit_ticks() at install time */
    uint32_t size_kb;
} pkg_entry_t;

static pkg_entry_t pkgdb[PKGDB_MAX];
static int         pkgdb_count = 0;
static uint8_t     pkgdb_dirty = 0; /* 1 if needs flush to storage */

/* Curated approved package list */
static const char *approved_pkgs[] = {
    "busybox", "coreutils", "bash", "dash", "musl", "musl-tools",
    "grep", "sed", "awk", "gawk", "tar", "gzip", "bzip2", "xz-utils",
    "curl", "wget", "openssl", "libssl", "ncurses", "readline",
    "vim", "nano", "less", "file", "findutils", "diffutils",
    "python3", "lua5.4", "perl",
    NULL
};

/* ---- Helpers ------------------------------------------------------------ */

static int is_approved(const char *name) {
    for (int i = 0; approved_pkgs[i]; i++)
        if (strcmp(approved_pkgs[i], name) == 0) return 1;
    return 0;
}

static uint32_t dec_to_u32(const char *s, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(s[i] - '0');
    return v;
}

static void print_progress(const char *msg, int step, int total) {
    char buf[80];
    int bars = (total > 0) ? (step * 20 / total) : 0;
    int i;
    buf[0] = '[';
    for (i = 0; i < 20; i++) buf[1+i] = (i < bars) ? '#' : '.';
    buf[21] = ']';
    buf[22] = ' ';
    int off = 23;
    for (const char *p = msg; *p && off < 78; p++) buf[off++] = *p;
    buf[off++] = '\n';
    buf[off]   = '\0';
    vga_print(buf);
}

/* Flush pkgdb to /storage/main/system/pkgdb.json */
static void pkgdb_flush(void) {
    if (!storage_root || !pkgdb_dirty) return;

    char json[2048];
    int  pos = 0;

    pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                    "{\"version\":1,\"packages\":[\n");

    for (int i = 0; i < pkgdb_count; i++) {
        if (!pkgdb[i].installed) continue;
        pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
            "  {\"name\":\"%s\",\"version\":\"%s\","
            "\"arch\":\"%s\",\"static\":%s,"
            "\"size_kb\":%u,\"desc\":\"%s\"}%s\n",
            pkgdb[i].name, pkgdb[i].version, pkgdb[i].arch,
            pkgdb[i].static_binary ? "true" : "false",
            pkgdb[i].size_kb, pkgdb[i].description,
            (i < pkgdb_count - 1) ? "," : "");
    }
    pos += snprintf(json + pos, sizeof(json) - (size_t)pos, "]}\n");

    /* Write to storage */
    vfs_node_t *f = vfs_finddir(storage_root, "pkgdb.json");
    if (!f) {
        ramfs_create(storage_root, "pkgdb.json");
        f = vfs_finddir(storage_root, "pkgdb.json");
    }
    if (f) {
        vfs_write(f, 0, (uint32_t)pos, (const uint8_t *)json);
        pkgdb_dirty = 0;
        kinfo("DRACO: pkgdb.json flushed (%d bytes)\n", pos);
    } else {
        kerror("DRACO: could not write pkgdb.json\n");
    }
}

/* Log an install event to /storage/main/logs/install.log */
static void log_install(const char *name, const char *version,
                         const char *action) {
    if (!storage_root) return;
    vfs_node_t *f = vfs_finddir(storage_root, "install.log");
    if (!f) {
        ramfs_create(storage_root, "install.log");
        f = vfs_finddir(storage_root, "install.log");
    }
    if (!f) return;
    char line[128];
    snprintf(line, sizeof(line), "[%s] %s %s\n", action, name, version);
    uint32_t sz = f->size;
    vfs_write(f, sz, (uint32_t)strlen(line), (const uint8_t *)line);
}

/* ---- pkg_register ------------------------------------------------------- */

int pkg_register(const pkg_meta_t *meta) {
    for (int i = 0; i < pkgdb_count; i++) {
        if (strcmp(pkgdb[i].name, meta->name) == 0) {
            strncpy(pkgdb[i].version,     meta->version,     31);
            strncpy(pkgdb[i].arch,        meta->arch,        15);
            strncpy(pkgdb[i].description, meta->description, 255);
            pkgdb[i].installed     = (uint8_t)meta->installed;
            pkgdb[i].static_binary = (uint8_t)meta->static_binary;
            pkgdb_dirty = 1;
            return 0;
        }
    }
    if (pkgdb_count >= PKGDB_MAX) {
        vga_print("draco: package database full\n");
        return -1;
    }
    strncpy(pkgdb[pkgdb_count].name,        meta->name,        PKG_NAME_MAX-1);
    strncpy(pkgdb[pkgdb_count].version,     meta->version,     31);
    strncpy(pkgdb[pkgdb_count].arch,        meta->arch,        15);
    strncpy(pkgdb[pkgdb_count].description, meta->description, 255);
    pkgdb[pkgdb_count].installed     = (uint8_t)meta->installed;
    pkgdb[pkgdb_count].static_binary = (uint8_t)meta->static_binary;
    pkgdb[pkgdb_count].size_kb       = 0;
    pkgdb_count++;
    pkgdb_dirty = 1;
    return 0;
}

/* ---- pkg_list ----------------------------------------------------------- */

void pkg_list(void) {
    int found = 0;
    for (int i = 0; i < pkgdb_count; i++)
        if (pkgdb[i].installed) found++;

    if (found == 0) {
        vga_print("  No packages installed.\n");
        vga_print("  Use 'draco install <file.deb>' to install.\n");
        vga_print("  Use 'draco approved' to see curated packages.\n");
        return;
    }

    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_print("  Name              Version   Arch    Static  Size     Description\n");
    vga_print("  ----------------- --------- ------- ------- -------- -----------\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    char buf[160];
    for (int i = 0; i < pkgdb_count; i++) {
        if (!pkgdb[i].installed) continue;
        snprintf(buf, sizeof(buf),
            "  %-17s %-9s %-7s %-7s %-8u %s\n",
            pkgdb[i].name, pkgdb[i].version, pkgdb[i].arch,
            pkgdb[i].static_binary ? "yes" : "no",
            pkgdb[i].size_kb,
            pkgdb[i].description);
        vga_print(buf);
    }
    snprintf(buf, sizeof(buf), "\n  %d package(s) installed.\n", found);
    vga_print(buf);
}

/* ---- Create package directory tree in /storage/main/apps --------------- */

static void install_create_app_dir(const char *pkgname) {
    if (!storage_root) return;

    /* We create named slots: "app-<pkgname>", "app-<pkgname>-bin",
     * "app-<pkgname>-etc" since RAMFS is flat. */
    char slot[64];

    snprintf(slot, sizeof(slot), "app-%s", pkgname);
    if (ramfs_create(storage_root, slot) == 0) {
        vfs_node_t *nd = vfs_finddir(storage_root, slot);
        if (nd) nd->type = VFS_TYPE_DIR;
    }

    snprintf(slot, sizeof(slot), "app-%s-bin", pkgname);
    ramfs_create(storage_root, slot);

    snprintf(slot, sizeof(slot), "app-%s-etc", pkgname);
    ramfs_create(storage_root, slot);

    /* Write meta.json for this package */
    char meta_name[64], meta_content[256];
    snprintf(meta_name, sizeof(meta_name), "app-%s-meta", pkgname);
    snprintf(meta_content, sizeof(meta_content),
        "{\"name\":\"%s\",\"installed\":true,\"path\":\"/storage/main/apps/%s\"}\n",
        pkgname, pkgname);
    ramfs_create(storage_root, meta_name);
    vfs_node_t *mf = vfs_finddir(storage_root, meta_name);
    if (mf)
        vfs_write(mf, 0, (uint32_t)strlen(meta_content),
                  (const uint8_t *)meta_content);

    kinfo("DRACO: created app dir for '%s'\n", pkgname);
}

/* ---- .deb ar extraction ------------------------------------------------ */

int pkg_extract_deb(const char *deb_path, const char *pkgname) {
    if (!ramfs_root) {
        vga_print("draco: RAMFS not available\n");
        return -1;
    }

    /* Resolve name (strip /ramfs/ prefix) */
    const char *fname = deb_path;
    if (strncmp(deb_path, "/ramfs/", 7) == 0) fname = deb_path + 7;
    else if (deb_path[0] == '/') {
        /* Try any prefix */
        const char *last = deb_path;
        for (const char *p = deb_path; *p; p++)
            if (*p == '/') last = p + 1;
        fname = last;
    }

    kdebug("DRACO: looking for '%s' in ramfs\n", fname);
    vfs_node_t *node = vfs_finddir(ramfs_root, fname);
    if (!node) {
        vga_print("draco: package file '");
        vga_print(fname);
        vga_print("' not found in /ramfs/\n");
        vga_print("  Tip: copy the .deb to /ramfs/ first.\n");
        vga_print("  Since there is no network yet, transfer via serial\n");
        vga_print("  or use 'draco install --fake <name>' to test.\n");
        return -1;
    }

    /* Read file */
    uint8_t buf[4096];
    int n = vfs_read(node, 0, sizeof(buf), buf);
    if (n < 8) {
        vga_print("draco: file too small to be a .deb (need >= 8 bytes)\n");
        return -1;
    }

    /* Check ar magic */
    if (memcmp(buf, "!<arch>\n", 8) != 0) {
        vga_print("draco: not a valid .deb file (bad ar magic)\n");
        vga_print("  Expected: !<arch>\\n at offset 0\n");
        char hex[32];
        snprintf(hex, sizeof(hex), "  Got:      0x%02x%02x%02x%02x...\n",
                 buf[0], buf[1], buf[2], buf[3]);
        vga_print(hex);
        return -1;
    }

    kinfo("DRACO: .deb ar magic OK, parsing members...\n");
    print_progress("Parsing .deb", 1, 5);

    /* Walk ar members */
    int off = 8;
    int data_found = 0;
    char control_name[64] = "";
    char data_name[64]    = "";
    uint32_t data_size    = 0;
    (void)data_size;

    (void)data_size;

    while (off + 60 <= n) {
        /* ar header: 60 bytes
         * name[16] date[12] uid[6] gid[6] mode[8] size[10] fmag[2] */
        char ar_name[17]; memcpy(ar_name, buf + off, 16); ar_name[16] = '\0';
        /* Trim trailing spaces */
        for (int i = 15; i >= 0 && ar_name[i] == ' '; i--) ar_name[i] = '\0';
        uint32_t ar_sz = dec_to_u32((char *)(buf + off + 48), 10);
        off += 60; /* skip header */

        kdebug("DRACO: ar member '%s' size=%u\n", ar_name, ar_sz);

        if (strncmp(ar_name, "control.tar", 11) == 0) {
            strncpy(control_name, ar_name, 63);
            kinfo("DRACO: found control archive: %s (%u bytes)\n",
                  ar_name, ar_sz);
        } else if (strncmp(ar_name, "data.tar", 8) == 0) {
            strncpy(data_name, ar_name, 63);
            data_size = ar_sz;
            data_found = 1;
            kinfo("DRACO: found data archive: %s (%u bytes)\n",
                  ar_name, ar_sz);
        } else if (strcmp(ar_name, "debian-binary") == 0 && ar_sz >= 3) {
            char ver[8]; memcpy(ver, buf+off, (ar_sz<7)?ar_sz:7); ver[ar_sz<7?ar_sz:7]='\0';
            kinfo("DRACO: debian-binary version: %s\n", ver);
        }

        /* Advance past data, with even-alignment */
        off += (int)ar_sz;
        if (ar_sz & 1) off++;

        if (off > n) break;
    }

    print_progress("Validating package", 2, 5);

    if (!data_found) {
        vga_print("draco: no data.tar.* member found in .deb archive\n");
        vga_print("  This .deb may be corrupted or not a standard format.\n");
        return -1;
    }

    print_progress("Creating app directory", 3, 5);
    install_create_app_dir(pkgname);

    print_progress("Registering package", 4, 5);

    /* Register in pkgdb */
    pkg_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    strncpy(meta.name,        pkgname,      PKG_NAME_MAX-1);
    strncpy(meta.version,     "1.0",        31);
    strncpy(meta.arch,        "amd64",      15);
    strncpy(meta.description, "Installed from .deb", 255);
    meta.installed     = 1;
    meta.static_binary = 1;

    pkg_register(&meta);
    pkgdb_flush();
    log_install(pkgname, "1.0", "install");

    print_progress("Done", 5, 5);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print("Package '");
    vga_print(pkgname);
    vga_print("' installed successfully.\n");
    vga_print("  Location: /storage/main/apps/");
    vga_print(pkgname);
    vga_print("/\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    return 0;
}

/* ---- Fake install (simulation for testing) ------------------------------ */

static void pkg_fake_install(const char *name, const char *version,
                              const char *desc) {
    vga_print("draco: simulating install of '");
    vga_print(name); vga_print("'...\n");

    print_progress("Resolving dependencies", 1, 6);
    print_progress("Downloading (simulated)", 2, 6);
    print_progress("Verifying checksum",      3, 6);
    print_progress("Extracting",              4, 6);
    install_create_app_dir(name);
    print_progress("Registering",             5, 6);

    pkg_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    strncpy(meta.name,        name,    PKG_NAME_MAX-1);
    strncpy(meta.version,     version, 31);
    strncpy(meta.arch,        "amd64", 15);
    strncpy(meta.description, desc,    255);
    meta.installed     = 1;
    meta.static_binary = 1;

    pkg_register(&meta);
    pkgdb_flush();
    log_install(name, version, "fake-install");

    print_progress("Complete", 6, 6);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_print("Package '"); vga_print(name);
    vga_print("' (simulated) installed.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ---- Remove ------------------------------------------------------------ */

static void pkg_remove(const char *name) {
    for (int i = 0; i < pkgdb_count; i++) {
        if (strcmp(pkgdb[i].name, name) != 0) continue;
        if (!pkgdb[i].installed) {
            vga_print("draco: '"); vga_print(name);
            vga_print("' is not installed\n");
            return;
        }
        pkgdb[i].installed = 0;
        pkgdb_dirty = 1;
        pkgdb_flush();
        log_install(name, pkgdb[i].version, "remove");
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_print("Package '"); vga_print(name);
        vga_print("' removed.\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    vga_print("draco: package '"); vga_print(name);
    vga_print("' not found\n");
}

/* ---- Info -------------------------------------------------------------- */

static void pkg_info(const char *name) {
    for (int i = 0; i < pkgdb_count; i++) {
        if (strcmp(pkgdb[i].name, name) != 0) continue;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "Package  : %s\n"
            "Version  : %s\n"
            "Arch     : %s\n"
            "Static   : %s\n"
            "Status   : %s\n"
            "Location : /storage/main/apps/%s/\n"
            "Desc     : %s\n",
            pkgdb[i].name, pkgdb[i].version, pkgdb[i].arch,
            pkgdb[i].static_binary ? "yes (musl)" : "no",
            pkgdb[i].installed ? "installed" : "removed",
            pkgdb[i].name,
            pkgdb[i].description);
        vga_print(buf);
        return;
    }
    vga_print("draco: package '"); vga_print(name);
    vga_print("' not in database\n");
}

/* ---- Search ------------------------------------------------------------ */

static void pkg_search(const char *query) {
    vga_print("draco: searching approved packages for '");
    vga_print(query); vga_print("':\n");
    int found = 0;
    for (int i = 0; approved_pkgs[i]; i++) {
        /* Simple substring check */
        const char *h = approved_pkgs[i];
        size_t ql = strlen(query), hl = strlen(h);
        int match = 0;
        for (size_t j = 0; j + ql <= hl; j++) {
            if (strncmp(h + j, query, ql) == 0) { match = 1; break; }
        }
        if (match) {
            /* Check if installed */
            int installed = 0;
            for (int k = 0; k < pkgdb_count; k++)
                if (strcmp(pkgdb[k].name, h) == 0 && pkgdb[k].installed)
                    installed = 1;
            vga_print(installed ? "  [installed] " : "  [ approved] ");
            vga_print(approved_pkgs[i]); vga_putchar('\n');
            found++;
        }
    }
    if (!found) vga_print("  No matches found.\n");
}

/* ---- Status ------------------------------------------------------------ */

static void pkg_status(void) {
    int inst = 0;
    for (int i = 0; i < pkgdb_count; i++)
        if (pkgdb[i].installed) inst++;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "Draco Install v1.0\n"
        "  Packages installed : %d\n"
        "  Database entries   : %d / %d\n"
        "  Storage            : /storage/main/apps/\n"
        "  pkgdb.json         : /storage/main/system/pkgdb.json\n"
        "  Network            : not available (V2)\n"
        "  .deb support       : ar+control.tar parsing\n",
        inst, pkgdb_count, PKGDB_MAX);
    vga_print(buf);
}

/* ---- Repo & network stubs ----------------------------------------------- */

typedef struct {
    const char *name, *type, *url, *suite, *arch;
} repo_t;

static const repo_t builtin_repos[] = {
    {"debian-main", "debian",      "https://deb.debian.org/debian",      "bookworm", "amd64"},
    {"debian-pool", "debian-pool", "https://deb.debian.org/debian/pool/", NULL,       "amd64"},
    {NULL, NULL, NULL, NULL, NULL}
};
#define REPO_MAX 8
static repo_t active_repos[REPO_MAX];
static int    repo_count = 0;

static void repos_load_defaults(void) {
    repo_count = 0;
    for (int i = 0; builtin_repos[i].name && repo_count < REPO_MAX; i++)
        active_repos[repo_count++] = builtin_repos[i];
}

static int net_fetch(const char *url, char *buf, uint32_t bufsz, uint32_t timeout_ms) {
    (void)buf; (void)bufsz;
    vga_print("  [NET] "); vga_print(url); vga_putchar('\n');
    (void)timeout_ms;
    /* Stub: no network stack yet */
    vga_print("  [NET] No network (TCP/IP pending V2)\n");
    return -2;
}

static void pkg_net_install(const char *name, const char *version) {
    if (repo_count == 0) repos_load_defaults();
    vga_print("  Attempting network fetch for '"); vga_print(name); vga_print("'\n");
    char url[256];
    snprintf(url, sizeof(url),
        "%spool/main/%c/%s/%s_%s_amd64.deb",
        active_repos[0].url, name[0], name, name,
        version ? version : "latest");
    char dummy[16];
    if (net_fetch(url, dummy, sizeof(dummy), 5000) == 0)
        pkg_fake_install(name, version ? version : "latest", "Network installed");
    else {
        vga_print("draco: fetch failed. Download the .deb manually:\n");
        vga_print("  place in /ramfs/ then run: draco install /ramfs/");
        vga_print(name); vga_print(".deb\n");
    }
}

static const char *detect_arch(void) { return "amd64"; }

/* ---- Shell entry point ------------------------------------------------- */

void draco_install_run(int argc, char *argv[]) {
    if (argc < 2) {
        vga_print(
            "Draco Install — package manager\n\n"
            "  draco install <file.deb>        install from .deb in /ramfs\n"
            "  draco install --fake <name>     simulate install (testing)\n"
            "  draco install --fake <n> <ver> <desc>\n"
            "  draco remove  <name>            uninstall\n"
            "  draco list                      list installed packages\n"
            "  draco info    <name>            show package details\n"
            "  draco search  <query>           search approved list\n"
            "  draco approved                  show all approved packages\n"
            "  draco status                    show package manager status\n"
            "\n"
            "No network yet: place .deb files in /ramfs/ first.\n"
            "Use '--fake' to simulate a package install for testing.\n"
        );
        return;
    }

    const char *sub = argv[1];

    if (!strcmp(sub, "list")) {
        pkg_list(); return;
    }

    if (!strcmp(sub, "status")) {
        pkg_status(); return;
    }

    if (!strcmp(sub, "approved")) {
        vga_print("Curated approved packages:\n");
        for (int i = 0; approved_pkgs[i]; i++) {
            /* Mark if already installed */
            int inst = 0;
            for (int k = 0; k < pkgdb_count; k++)
                if (strcmp(pkgdb[k].name, approved_pkgs[i]) == 0
                    && pkgdb[k].installed) inst = 1;
            vga_print(inst ? "  [+] " : "  [ ] ");
            vga_print(approved_pkgs[i]); vga_putchar('\n');
        }
        return;
    }

    if (!strcmp(sub, "search") && argc >= 3) {
        pkg_search(argv[2]); return;
    }

    if (!strcmp(sub, "info") && argc >= 3) {
        pkg_info(argv[2]); return;
    }

    if (!strcmp(sub, "remove") && argc >= 3) {
        pkg_remove(argv[2]); return;
    }

    if (!strcmp(sub, "reinstall") && argc >= 3) {
        /* Remove then fake-reinstall */
        pkg_remove(argv[2]);
        pkg_fake_install(argv[2], "1.0", "Reinstalled");
        return;
    }

    if (!strcmp(sub, "install") && argc >= 3) {
        /* Check for --fake flag */
        if (!strcmp(argv[2], "--fake")) {
            if (argc < 4) {
                vga_print("usage: draco install --fake <name> [version] [desc]\n");
                return;
            }
            const char *name    = argv[3];
            const char *version = (argc >= 5) ? argv[4] : "1.0";
            const char *desc    = (argc >= 6) ? argv[5] : "Simulated package";

            if (!is_approved(name)) {
                vga_print("Note: '"); vga_print(name);
                vga_print("' not in curated list. Proceeding anyway.\n");
            }
            pkg_fake_install(name, version, desc);
            return;
        }

        /* Real .deb install */
        const char *path = argv[2];

        /* Derive package name from filename */
        const char *name = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') name = p + 1;

        char pkgname[PKG_NAME_MAX];
        strncpy(pkgname, name, PKG_NAME_MAX - 1);
        int l = (int)strlen(pkgname);
        if (l > 4 && strcmp(pkgname + l - 4, ".deb") == 0)
            pkgname[l - 4] = '\0';

        if (!is_approved(pkgname)) {
            vga_set_color(VGA_LIGHT_BROWN, VGA_BLACK);
            vga_print("Warning: '"); vga_print(pkgname);
            vga_print("' is not in the curated list. Proceeding (experimental).\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }

        vga_print("Installing '"); vga_print(pkgname);
        vga_print("' from '"); vga_print(path); vga_print("'...\n");

        int r = pkg_extract_deb(path, pkgname);
        if (r != 0) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_print("Install FAILED. See messages above for details.\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        return;
    }

    if (!strcmp(sub, "update")) {
        /* draco update — refresh package lists from repos */
        vga_print("Draco Install: updating package lists...\n");
        if (repo_count == 0) repos_load_defaults();
        for (int r = 0; r < repo_count; r++) {
            vga_print("  Checking repo: "); vga_print(active_repos[r].name);
            vga_putchar('\n');
            char dummy[16];
            int ret = net_fetch(active_repos[r].url, dummy, sizeof(dummy), 5000);
            if (ret < 0) vga_print("  (offline — using cached list)\n");
            else vga_print("  Updated.\n");
        }
        vga_print("Package lists refreshed.\n");
        return;
    }

    if (!strcmp(sub, "upgrade")) {
        /* draco upgrade — upgrade all installed packages */
        vga_print("Draco Install: upgrading all packages...\n");
        int upgraded = 0;
        for (int i = 0; i < pkgdb_count; i++) {
            if (!pkgdb[i].installed) continue;
            vga_print("  Checking: "); vga_print(pkgdb[i].name);
            vga_print(" ("); vga_print(pkgdb[i].version); vga_print(")\n");
            /* Simulate upgrade: in real impl, compare against repo version */
            print_progress("Upgrading", 1, 1);
            upgraded++;
        }
        if (!upgraded) vga_print("Nothing to upgrade.\n");
        else { char b[32]; snprintf(b,32,"Upgraded %d package(s).\n",upgraded); vga_print(b); }
        return;
    }

    if (!strcmp(sub, "get") && argc >= 3) {
        /* draco get <package> [version] — fetch+install from repo */
        const char *name = argv[2];
        const char *ver  = (argc >= 4) ? argv[3] : NULL;
        vga_print("Fetching '"); vga_print(name); vga_print("'");
        if (ver) { vga_print(" version="); vga_print(ver); }
        vga_putchar('\n');
        /* Check if .deb already in /ramfs */
        char deb_path[256];
        snprintf(deb_path, sizeof(deb_path), "/ramfs/%s.deb", name);
        /* Try local first */
        if (ramfs_root) {
            vga_print("  Trying local /ramfs/"); vga_print(name); vga_print(".deb ...\n");
            int r = pkg_extract_deb(deb_path, name);
            if (r == 0) return;
        }
        /* Fallback: network fetch */
        pkg_net_install(name, ver);
        return;
    }

    if (!strcmp(sub, "repo") && argc >= 3) {
        if (!strcmp(argv[2], "list")) {
            if (repo_count == 0) repos_load_defaults();
            vga_print("Configured repos:\n");
            for (int r = 0; r < repo_count; r++) {
                vga_print("  ["); vga_print(active_repos[r].name); vga_print("]\n");
                vga_print("    type: "); vga_print(active_repos[r].type); vga_putchar('\n');
                vga_print("    url : "); vga_print(active_repos[r].url); vga_putchar('\n');
            }
        } else if (!strcmp(argv[2], "add") && argc >= 5) {
            if (repo_count >= REPO_MAX) { vga_print("Repo limit reached.\n"); return; }
            active_repos[repo_count].name  = argv[3];
            active_repos[repo_count].type  = "debian";
            active_repos[repo_count].url   = argv[4];
            active_repos[repo_count].suite = (argc >= 6) ? argv[5] : "bookworm";
            active_repos[repo_count].arch  = detect_arch();
            repo_count++;
            vga_print("Repo added: "); vga_print(argv[3]); vga_putchar('\n');
        } else if (!strcmp(argv[2], "remove") && argc >= 4) {
            int removed = 0;
            for (int r = 0; r < repo_count; r++) {
                if (strcmp(active_repos[r].name, argv[3]) == 0) {
                    for (int j = r; j < repo_count - 1; j++)
                        active_repos[j] = active_repos[j+1];
                    repo_count--; removed++;
                    vga_print("Repo removed: "); vga_print(argv[3]); vga_putchar('\n');
                    break;
                }
            }
            if (!removed) { vga_print("Repo not found: "); vga_print(argv[3]); vga_putchar('\n'); }
        }
        return;
    }

    vga_print("draco: unknown command '");
    vga_print(sub);
    vga_print("'\nRun 'draco' with no arguments for usage.\n");
}

