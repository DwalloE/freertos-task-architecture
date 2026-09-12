/*
 * crash.c - the stack-overflow demo. `crash` spawns a victim task with a
 * deliberately small stack and recurses until the frames run past the
 * end, overwriting the canary bytes FreeRTOS painted at the stack's far
 * edge. The next context switch runs the canary check
 * (CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY) and panics NAMING THE
 * TASK - that panic text is what the second CI scenario requires, which
 * turns the safety net from a config line into a tested claim.
 *
 * The recursion watches its own high-water mark to walk NEAR the edge,
 * then one deliberately-oversized final frame plunges past it, so the
 * overshoot beyond the canary is a few hundred bytes, not kilobytes of
 * heap scribbling - we want the canary panic, not a random heap crash.
 * (First version stopped while headroom remained and never overflowed at
 * all - caught by Elias's screen recording of the browser demo, where
 * `crash` parked forever instead of panicking. README bug gallery.)
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_tasks.h"
#include "stacks.h"

/* GCC is right that this never terminates normally: one branch recurses,
 * the other parks forever waiting for the canary panic. That is the demo.
 * Silence exactly this diagnostic, here only. */
/* The final frame: bigger than any headroom eat_stack leaves behind, so
 * writing it provably crosses the canary at the stack's far edge. Then
 * park - the very next context switch runs the canary check and panics. */
static void __attribute__((noinline)) plunge(void)
{
    volatile uint8_t last[512];
    for (unsigned i = 0; i < sizeof last; i++)
        last[i] = 0xEE;
    for (;;) vTaskDelay(pdMS_TO_TICKS(50));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static void __attribute__((noinline)) eat_stack(uint32_t depth)
{
    volatile uint8_t frame[128];
    for (unsigned i = 0; i < sizeof frame; i++)
        frame[i] = (uint8_t)depth;              /* really touch every byte */

    if (uxTaskGetStackHighWaterMark(NULL) > sizeof frame + 96)
        eat_stack(depth + 1);                   /* walk down to the edge... */
    else
        plunge();                               /* ...and step over it */
}
#pragma GCC diagnostic pop

static void victim_task(void *arg)
{
    (void)arg;
    printf("crash: victim task up with %d bytes of stack, recursing past "
           "the end on purpose\n", VICTIM_STACK_BYTES);
    eat_stack(1);
    vTaskDelete(NULL);                          /* not reached */
}

void crash_now(void)
{
    /* Core 1 so the panic dump clearly shows the overflow on the data-
     * plane core while the shell (core 0) keeps its last words flushed. */
    xTaskCreatePinnedToCore(victim_task, "victim", VICTIM_STACK_BYTES,
                            NULL, 4, NULL, 1);
}
