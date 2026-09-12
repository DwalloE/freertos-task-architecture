/*
 * crash.c - the stack-overflow demo. `crash` spawns a victim task with a
 * deliberately small stack and recurses until the frames run past the
 * end, overwriting the canary bytes FreeRTOS painted at the stack's far
 * edge. The next context switch runs the canary check
 * (CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY) and panics NAMING THE
 * TASK - that panic text is what the second CI scenario requires, which
 * turns the safety net from a config line into a tested claim.
 *
 * The recursion watches its own high-water mark and stops adding frames
 * only when the headroom is already smaller than a frame, so the
 * overshoot past the canary is one frame (~200 bytes), not kilobytes of
 * heap scribbling - we want the canary panic, not a random heap crash.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_tasks.h"
#include "stacks.h"

static void __attribute__((noinline)) eat_stack(uint32_t depth)
{
    volatile uint8_t frame[128];
    for (unsigned i = 0; i < sizeof frame; i++)
        frame[i] = (uint8_t)depth;              /* really touch every byte */

    if (uxTaskGetStackHighWaterMark(NULL) > sizeof frame + 96) {
        eat_stack(depth + 1);
    } else {
        /* Headroom is now less than one more frame: the frame above just
         * crossed (or grazed) the canary. Yield so the scheduler switches
         * us out and the canary check runs. */
        for (;;) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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
