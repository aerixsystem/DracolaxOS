/* tests/test_keyboard.c — Unit tests for keyboard ring buffer */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#define KB_BUF 128
static uint8_t ring[KB_BUF];
static uint8_t r_head = 0, r_tail = 0;

static void ring_push(uint8_t c) {
    uint8_t next = (r_tail + 1) & (KB_BUF - 1);
    if (next != r_head) { ring[r_tail] = c; r_tail = next; }
}

static char ring_pop(void) {
    if (r_head == r_tail) return 0;
    uint8_t c = ring[r_head];
    r_head = (r_head + 1) & (KB_BUF - 1);
    return (char)c;
}

int main(void) {
    int pass = 0, fail = 0;
#define CHECK(c,m) do{ if(c){printf("[PASS] %s\n",m);pass++;}else{printf("[FAIL] %s\n",m);fail++;} }while(0)

    /* Test 1: basic push/pop */
    ring_push('A');
    CHECK(ring_pop() == 'A', "push/pop ASCII char");

    /* Test 2: empty ring returns 0 */
    CHECK(ring_pop() == 0, "empty ring returns 0");

    /* Test 3: Tab and Enter round-trip */
    ring_push('\t');
    ring_push('\n');
    CHECK(ring_pop() == '\t', "Tab character survives ring");
    CHECK(ring_pop() == '\n', "Enter character survives ring");

    /* Test 4: special key code (>= 0x80) round-trip */
    ring_push(0x80); /* KB_KEY_UP */
    uint8_t got = (uint8_t)ring_pop();
    CHECK(got == 0x80, "KB_KEY_UP (0x80) survives ring");

    /* Test 5: fill ring to capacity without overflow */
    r_head = r_tail = 0;
    for (int i = 0; i < KB_BUF - 1; i++) ring_push((uint8_t)((i % 126) + 1));
    /* One more push should be silently dropped (ring full) */
    ring_push(0x41);
    int count = 0;
    while (ring_pop()) count++;
    /* Should have exactly KB_BUF-1 items (the extra was dropped) */
    CHECK(count == KB_BUF - 1, "ring drops excess when full (no overflow)");

    /* Test 6: wrap-around correctness */
    r_head = r_tail = 120;
    ring_push('X'); ring_push('Y'); ring_push('Z');
    CHECK(ring_pop()=='X' && ring_pop()=='Y' && ring_pop()=='Z',
          "ring wrap-around preserves order");

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
