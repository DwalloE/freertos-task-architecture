/*
 * crash.c - the stack-overflow demo. `crash` spawns a victim task with a
 * deliberately small stack that scribbles from its current stack position
 * down THROUGH the canary bytes FreeRTOS painted at the far edge -
 * exactly the damage runaway recursion does. The next context switch
 * runs the canary check (CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY) and
 * panics NAMING THE TASK - that panic text is what the second CI
 * scenario requires, which turns the safety net from a config line into
 * a tested claim.
 *
 * Third design, and the two dead ones are the README bug gallery's best
 * entries: v1 recursed by high-water mark and stopped WHILE HEADROOM
 * REMAINED - never touched the canary, never panicked (caught in Elias's
 * screen recording). v2 plunged a blind 512-byte frame "just past" the
 * edge - overshot the stack into the heap, wedged the victim spinning
 * (task WDT: "CPU 1: victim"), starved the aggregator, still no canary
 * panic (CI run 34690787832). v3 asks the kernel where the stack
 * actually ends (vTaskGetInfo -> pxStackBase, available because
 * CONFIG_FREERTOS_USE_TRACE_FACILITY=y) and stomps from here exactly TO
 * that base: a full, genuine overflow of everything below the live
 * frames, canary included, and not one byte of collateral heap damage.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_tasks.h"
#include "stacks.h"

static void victim_task(void *arg)
{
    (void)arg;
    printf("crash: victim task up with %d bytes of stack, scribbling past "
           "the end on purpose\n", VICTIM_STACK_BYTES);

    /* Where does this task's stack really end? pxStackBase is its lowest
     * address - the canary lives in the first bytes there. */
    TaskStatus_t st;
    vTaskGetInfo(NULL, &st, pdFALSE, eRunning);
    volatile uint8_t *base = (volatile uint8_t *)st.pxStackBase;

    /* From (roughly) the current stack pointer, walk down to base,
     * overwriting everything: the untouched 0xa5 fill, then the canary.
     * Writing below SP is safe here - nothing lives there until the
     * vTaskDelay below builds its frames in the freshly-stomped area. */
    volatile uint8_t marker;
    volatile uint8_t *p = &marker;
    while (p > base)
        *--p = 0xEE;

    /* Switch out; the scheduler's canary check does the rest. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(50));
}

void crash_now(void)
{
    /* Core 1 so the panic dump clearly shows the overflow on the data-
     * plane core while the shell (core 0) keeps its last words flushed. */
    xTaskCreatePinnedToCore(victim_task, "victim", VICTIM_STACK_BYTES,
                            NULL, 4, NULL, 1);
}
