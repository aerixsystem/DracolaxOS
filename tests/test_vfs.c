/* tests/test_vfs.c — VFS + RAMFS tests */
#include "../kernel/fs/vfs.h"
#include "../kernel/fs/ramfs.h"
#include "../kernel/log.h"
#include "../kernel/klibc.h"

static int pass, fail;

static void check(const char *name, int cond) {
    if (cond) { kinfo("  PASS: %s\n", name); pass++; }
    else       { kerror("  FAIL: %s\n", name); fail++; }
}

void test_vfs(void) {
    kinfo("=== VFS + RAMFS tests ===\n");
    pass = fail = 0;

    vfs_node_t *root = ramfs_init();
    check("ramfs_init returns non-NULL", root != NULL);
    if (!root) { kerror("Cannot continue\n"); return; }

    /* Mount */
    int r = vfs_mount("/test", root);
    check("vfs_mount succeeds", r == 0);

    /* Create file */
    r = ramfs_create(root, "hello.txt");
    check("ramfs_create succeeds", r == 0);

    /* Duplicate create fails */
    r = ramfs_create(root, "hello.txt");
    check("duplicate create fails", r != 0);

    /* Find file */
    vfs_node_t *f = vfs_finddir(root, "hello.txt");
    check("finddir finds file", f != NULL);

    /* Write */
    const char *msg = "Hello, kernel!";
    int n = vfs_write(f, 0, (uint32_t)strlen(msg), (const uint8_t *)msg);
    check("write returns byte count", n == (int)strlen(msg));
    check("file size updated", f->size == (uint32_t)strlen(msg));

    /* Read back */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    n = vfs_read(f, 0, (uint32_t)strlen(msg), buf);
    check("read returns byte count", n == (int)strlen(msg));
    check("read content matches", memcmp(buf, msg, (size_t)strlen(msg)) == 0);

    /* Partial read */
    memset(buf, 0, sizeof(buf));
    n = vfs_read(f, 7, 6, buf);  /* "kernel" */
    check("partial read ok", n == 6 && memcmp(buf, "kernel", 6) == 0);

    /* Readdir */
    char name[VFS_NAME_MAX];
    r = vfs_readdir(root, 0, name, sizeof(name));
    check("readdir index 0 ok", r == 0);
    check("readdir returns correct name", strcmp(name, "hello.txt") == 0);

    r = vfs_readdir(root, 1, name, sizeof(name));
    check("readdir past end returns -1", r == -1);

    /* Overwrite */
    const char *msg2 = "World!";
    vfs_write(f, 0, (uint32_t)strlen(msg2), (const uint8_t *)msg2);
    memset(buf, 0, sizeof(buf));
    vfs_read(f, 0, (uint32_t)strlen(msg2), buf);
    check("overwrite works", memcmp(buf, msg2, strlen(msg2)) == 0);

    /* Delete */
    r = ramfs_delete(root, "hello.txt");
    check("ramfs_delete succeeds", r == 0);

    f = vfs_finddir(root, "hello.txt");
    check("deleted file not found", f == NULL);

    r = ramfs_delete(root, "hello.txt");
    check("delete non-existent returns -1", r == -1);

    /* vfs_open through mount point */
    ramfs_create(root, "data");
    vfs_node_t *via_vfs = vfs_open("/test/data");
    check("vfs_open resolves through mount", via_vfs != NULL);

    kinfo("VFS: %d passed, %d failed\n", pass, fail);
}
