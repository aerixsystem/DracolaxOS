/* tests/test_log_rotation.c — Unit tests for log rotation logic */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define KLOG_MAX_LINES 9000

typedef struct {
    int line_count;
    int file_idx;
    char cur_path[128];
} log_chan_t;

static void make_log_path(log_chan_t *c, int idx, char *out, int sz) {
    snprintf(out, (size_t)sz, "storage/main/logs/kernel/%04d-01-01_%06d.log", 2026, idx);
}

static void rotate_if_needed(log_chan_t *c) {
    if (c->line_count < KLOG_MAX_LINES) return;
    c->file_idx++;
    c->line_count = 0;
    make_log_path(c, c->file_idx, c->cur_path, (int)sizeof(c->cur_path));
}

int main(void) {
    int pass = 0, fail = 0;
#define CHECK(cond,msg) do{if(cond){printf("[PASS] %s\n",msg);pass++;}else{printf("[FAIL] %s\n",msg);fail++;}}while(0)

    log_chan_t ch; memset(&ch, 0, sizeof(ch));
    make_log_path(&ch, 0, ch.cur_path, sizeof(ch.cur_path));

    /* Test 1: initial path contains 2026 */
    CHECK(strstr(ch.cur_path, "2026") != NULL, "log path contains year");

    /* Test 2: no rotation below threshold */
    ch.line_count = KLOG_MAX_LINES - 1;
    rotate_if_needed(&ch);
    CHECK(ch.file_idx == 0, "no rotation before 9000 lines");

    /* Test 3: rotation at threshold */
    ch.line_count = KLOG_MAX_LINES;
    rotate_if_needed(&ch);
    CHECK(ch.file_idx == 1, "rotation at 9000 lines");
    CHECK(ch.line_count == 0, "line count reset after rotation");

    /* Test 4: new path differs */
    char old[128]; strcpy(old, ch.cur_path);
    ch.line_count = KLOG_MAX_LINES;
    rotate_if_needed(&ch);
    CHECK(strcmp(old, ch.cur_path) != 0, "rotated path is different from previous");

    /* Test 5: sequential rotations produce sequential indices */
    CHECK(ch.file_idx == 2, "sequential rotation increments file_idx");

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
