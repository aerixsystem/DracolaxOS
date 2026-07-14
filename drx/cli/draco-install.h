/* userland/tools/draco-install/draco-install.h — Draco package manager */
#ifndef DRACO_INSTALL_H
#define DRACO_INSTALL_H

#define PKG_DB_PATH   "/storage/main/system/pkgdb.json"
#define PKG_APPS_PATH "/storage/main/apps"
#define PKGDB_MAX     32
#define PKG_NAME_MAX  128

typedef struct {
    char name[PKG_NAME_MAX];
    char version[32];
    char arch[16];       /* "i386", "amd64", "all" */
    char depends[256];
    char description[256];
    int  installed;
    int  static_binary;  /* 1 = musl/static; 0 = dynamic (experimental) */
} pkg_meta_t;

/* Shell entry point: draco install/remove/list */
void draco_install_run(int argc, char *argv[]);

/* Internal API */
int  pkg_extract_deb(const char *deb_path, const char *pkg_name);
int  pkg_register(const pkg_meta_t *meta);
void pkg_list(void);

#endif /* DRACO_INSTALL_H */
