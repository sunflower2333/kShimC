/* SPDX-License-Identifier: MIT */
/* Serial fallback boot menu (user/bootmenu/bootmenu.c) against scripted
 * console input, volume/power key events and a virtual millisecond clock. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <kshim/boot.h>
#include <kshim/console.h>
#include <kshim/input.h>
#include <kshim/time.h>
#include <kshim/user.h>

#define GUARD_MS 100000u

struct key_step { uint64_t at_ms; kshim_key_event_type_t type; kshim_key_action_t action; };
struct char_step { uint64_t at_ms; char c; };

static uint64_t now_ms;
static int keys_init_status;
static const struct key_step *keys; static size_t key_count, key_next;
static const struct char_step *chars; static size_t char_count, char_next;
static unsigned key_polls;
static unsigned waits, stalls;
static uint64_t max_sleep_ms;

static void guard(void)
{
    if (now_ms > GUARD_MS) {
        fprintf(stderr, "menu did not return within %u ms\n", GUARD_MS);
        abort();
    }
}
uint64_t get_current_time() { return now_ms; }
uint64_t delay(uint64_t micro_seconds)
{
    now_ms += micro_seconds / 1000u ? micro_seconds / 1000u : 1u;
    guard();
    return micro_seconds;
}
/* Sleep to the deadline, or until the next scripted input arrives (as its
 * interrupt would end the wait). The menus must make progress: a wait that
 * cannot advance the clock more than a few times in a row is a busy loop. */
void kshim_wait_event(uint64_t deadline_ms)
{
    uint64_t next = deadline_ms;
    ++waits;
    if (key_next < key_count && keys[key_next].at_ms > now_ms && keys[key_next].at_ms < next)
        next = keys[key_next].at_ms;
    if (char_next < char_count && chars[char_next].at_ms > now_ms && chars[char_next].at_ms < next)
        next = chars[char_next].at_ms;
    if (next <= now_ms) {
        if (++stalls > 3) {
            fprintf(stderr, "menu spins without sleeping at %llu ms\n", (unsigned long long)now_ms);
            abort();
        }
        return;
    }
    stalls = 0;
    if (deadline_ms - now_ms > max_sleep_ms) max_sleep_ms = deadline_ms - now_ms;
    now_ms = next;
    guard();
}
void kshim_console_write(const char *buffer, uint32_t length) { (void)buffer; (void)length; }
int kshim_console_try_read(char *character)
{
    if (char_next < char_count && chars[char_next].at_ms <= now_ms) {
        *character = chars[char_next++].c;
        return 1;
    }
    return 0;
}
int kshim_input_keys_init(const void *fdt) { (void)fdt; return keys_init_status; }
kshim_key_source_t *kshim_input_keys(void) { return (kshim_key_source_t *)&key_polls; }
int kshim_key_source_poll(kshim_key_source_t *source, kshim_key_event_t *event)
{
    assert(source == (kshim_key_source_t *)&key_polls);
    ++key_polls;
    if (key_next < key_count && keys[key_next].at_ms <= now_ms) {
        const struct key_step *k = &keys[key_next++];
        *event = (kshim_key_event_t){ .type = k->type, .action = k->action,
                                      .pressed = k->type == KSHIM_KEY_EVENT_PRESS };
        return KSHIM_KEYS_EVENT;
    }
    return KSHIM_KEYS_NO_EVENT; /* *event deliberately untouched */
}
int kshim_menu_activate(kshim_menu_action_t action) { (void)action; return 0; }

static const char *const names[] = {"Linux", "Recovery", "Other"};

static size_t run(const struct key_step *k, size_t nk, const struct char_step *c, size_t nc,
                  size_t initial, uint32_t timeout_ms)
{
    size_t selected = SIZE_MAX;
    now_ms = 0; key_polls = 0; waits = 0; stalls = 0; max_sleep_ms = 0;
    keys = k; key_count = nk; key_next = 0;
    chars = c; char_count = nc; char_next = 0;
    assert(kshim_user_select(0, names, 3, initial, timeout_ms, &selected) == 0);
    assert(selected < 3);
    return selected;
}

#define PRESS(t, a) { t, KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_##a }
#define RELEASE(t, a) { t, KSHIM_KEY_EVENT_RELEASE, KSHIM_KEY_ACTION_##a }
#define N(x) (sizeof(x) / sizeof((x)[0]))

int main(void)
{
    keys_init_status = 0;

    /* Volume down then power selects the next entry before the timeout. */
    static const struct key_step down_select[] = {PRESS(10, DOWN), PRESS(20, SELECT)};
    assert(run(down_select, N(down_select), NULL, 0, 0, 1000) == 1);
    assert(now_ms < 1000);

    /* One press moves exactly one step, however many idle polls follow
     * (the old check re-applied the stale last event on every idle poll). */
    static const struct key_step one_down[] = {PRESS(10, DOWN), PRESS(200, SELECT)};
    assert(run(one_down, N(one_down), NULL, 0, 0, 1000) == 1);

    /* Up from the first entry wraps to the last. */
    static const struct key_step up_select[] = {PRESS(10, UP), PRESS(20, SELECT)};
    assert(run(up_select, N(up_select), NULL, 0, 0, 1000) == 2);

    /* Release edges move nothing; only the press edges count. */
    static const struct key_step releases[] = {PRESS(10, DOWN), RELEASE(15, DOWN),
                                               RELEASE(18, DOWN), PRESS(20, SELECT)};
    assert(run(releases, N(releases), NULL, 0, 0, 1000) == 1);

    /* A key press cancels autoboot: at 100 ms the menu would have booted
     * entry 1; instead it waits for the serial choice at 500 ms. */
    static const struct key_step down_only[] = {PRESS(10, DOWN)};
    static const struct char_step one_enter[] = {{500, '1'}, {505, '\r'}};
    assert(run(down_only, N(down_only), one_enter, N(one_enter), 0, 100) == 0);
    assert(now_ms >= 505);

    /* Typing a digit cancels autoboot too. */
    static const struct char_step slow_three[] = {{10, '3'}, {400, '\r'}};
    assert(run(NULL, 0, slow_three, N(slow_three), 0, 100) == 2);
    assert(now_ms >= 400);

    /* No interaction: the default entry boots once the timeout expires. */
    assert(run(NULL, 0, NULL, 0, 1, 100) == 1);
    assert(now_ms >= 100 && now_ms < 120);
    /* It sleeps between polls (no busy loop), never past the 5 ms input
     * poll while input is polled, and wakes exactly at the deadline. */
    assert(waits >= 100 / 5 && waits <= 100 / 5 + 1);
    assert(max_sleep_ms == 5);
    assert(run(NULL, 0, NULL, 0, 2, 103) == 2);
    assert(now_ms == 103);

    /* Escape restores the default. */
    static const struct key_step down_then_esc[] = {PRESS(10, DOWN)};
    static const struct char_step esc[] = {{20, 27}};
    assert(run(down_then_esc, N(down_then_esc), esc, N(esc), 0, 1000) == 0);

    /* Without a key source the menu never polls keys. */
    keys_init_status = -1;
    static const struct char_step two_enter[] = {{10, '2'}, {11, '\n'}};
    assert(run(down_select, N(down_select), two_enter, N(two_enter), 0, 1000) == 1);
    assert(key_polls == 0);

    puts("bootmenu_test: ok");
    return 0;
}
