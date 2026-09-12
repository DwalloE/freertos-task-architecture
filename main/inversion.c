/*
 * inversion.c - priority inversion, measured instead of narrated.
 *
 * Three short-lived tasks, ALL pinned to core 1: inversion needs the
 * middle-priority spinner to actually starve the lock holder, and on a
 * dual-core part a task that can migrate simply runs on the other core
 * and the effect evaporates. One core, on purpose.
 *
 *   low  (prio 5): takes the lock, then does INV_HOLD_MS of busy work
 *   mid  (prio 6): busy-spins INV_SPIN_MS - the innocent bystander
 *   high (prio 8): blocks on the lock and measures how long
 *
 * Round 1 uses a binary semaphore: no priority inheritance, so mid
 * preempts low while low holds the lock, and high's blocked time
 * balloons to roughly the whole spin. Round 2 uses a mutex: low
 * inherits prio 8 the moment high blocks, finishes its hold un-preempted,
 * and the spike collapses to about the hold time.
 *
 * The experiment priorities (5/6/8) sit above the pipeline tasks (3/4)
 * so background traffic cannot blur the measurement. Times come from
 * esp_timer_get_time() (microseconds, independent of the 100 Hz tick).
 * The firmware computes the verdict itself and prints a distinctive
 * line - "inv: inheritance OK" - because the CI scenario can only match
 * text, not compare numbers (03's `-> plausible` pattern).
 */
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include "app_tasks.h"
#include "stacks.h"

#define INV_CORE      1
#define INV_LOW_PRIO  5
#define INV_MID_PRIO  6
#define INV_HIGH_PRIO 8
#define INV_HOLD_MS   50
#define INV_SPIN_MS   300

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;        /* counting: each task gives once */
    volatile bool     low_has_lock;
    volatile int64_t  blocked_us;  /* written by high, read after done x3 */
} inv_ctx_t;

static void busy_wait_ms(int32_t ms)
{
    int64_t t_end = esp_timer_get_time() + (int64_t)ms * 1000;
    volatile uint32_t sink = 0;
    while (esp_timer_get_time() < t_end) sink++;
}

static void inv_low_task(void *arg)
{
    inv_ctx_t *ctx = arg;
    xSemaphoreTake(ctx->lock, portMAX_DELAY);   /* uncontended: returns at once */
    ctx->low_has_lock = true;
    busy_wait_ms(INV_HOLD_MS);                  /* "work" done under the lock */
    xSemaphoreGive(ctx->lock);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void inv_high_task(void *arg)
{
    inv_ctx_t *ctx = arg;
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->blocked_us = esp_timer_get_time() - t0;
    xSemaphoreGive(ctx->lock);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static void inv_mid_task(void *arg)
{
    inv_ctx_t *ctx = arg;
    busy_wait_ms(INV_SPIN_MS);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

/* Returns high's blocked time in us, or -1 if the round wedged. */
static int64_t inv_round(bool use_mutex)
{
    inv_ctx_t ctx = { 0 };

    if (use_mutex) {
        ctx.lock = xSemaphoreCreateMutex();
    } else {
        ctx.lock = xSemaphoreCreateBinary();
        xSemaphoreGive(ctx.lock);               /* binary sems start empty */
    }
    ctx.done = xSemaphoreCreateCounting(3, 0);

    xTaskCreatePinnedToCore(inv_low_task, "inv_low", INV_TASK_STACK_BYTES,
                            &ctx, INV_LOW_PRIO, NULL, INV_CORE);
    /* Let low actually acquire before the waiter shows up. */
    while (!ctx.low_has_lock) vTaskDelay(pdMS_TO_TICKS(10));

    xTaskCreatePinnedToCore(inv_high_task, "inv_high", INV_TASK_STACK_BYTES,
                            &ctx, INV_HIGH_PRIO, NULL, INV_CORE);
    /* One tick for high to preempt and block on the lock... */
    vTaskDelay(pdMS_TO_TICKS(10));
    /* ...and now the bystander arrives. */
    xTaskCreatePinnedToCore(inv_mid_task, "inv_mid", INV_TASK_STACK_BYTES,
                            &ctx, INV_MID_PRIO, NULL, INV_CORE);

    bool ok = true;
    for (int i = 0; i < 3; i++)
        if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(2000)) != pdTRUE) ok = false;

    vSemaphoreDelete(ctx.lock);
    vSemaphoreDelete(ctx.done);
    return ok ? ctx.blocked_us : -1;
}

void inversion_run(void)
{
    printf("inv: lock held %d ms by prio-%d task; prio-%d spinner runs %d ms; "
           "prio-%d waiter measured; all on core %d\n",
           INV_HOLD_MS, INV_LOW_PRIO, INV_MID_PRIO, INV_SPIN_MS,
           INV_HIGH_PRIO, INV_CORE);

    int64_t sem_us = inv_round(false);
    int64_t mut_us = inv_round(true);

    if (sem_us < 0 || mut_us < 0) {
        printf("inv: EXPERIMENT WEDGED (sem=%lld us, mutex=%lld us)\n",
               (long long)sem_us, (long long)mut_us);
        return;
    }

    printf("inv: binary semaphore (no inheritance): high-prio blocked %lld us\n",
           (long long)sem_us);
    printf("inv: mutex (priority inheritance):      high-prio blocked %lld us\n",
           (long long)mut_us);
    printf("inv: ratio %lld.%lldx\n",
           (long long)(sem_us / mut_us),
           (long long)((sem_us * 10 / mut_us) % 10));

    /* The claim, computed where the numbers are: the semaphore round must
     * show the spike (blocked ~ spin time) and the mutex round must not
     * (blocked ~ hold time). The failure string shares no substring with
     * the pass string, so a scenario cannot match it by accident. */
    bool spike_present   = sem_us > (INV_SPIN_MS * 1000) / 2;
    bool spike_collapsed = mut_us < (INV_HOLD_MS * 1000) * 2 + 20000
                        && mut_us * 3 < sem_us;
    if (spike_present && spike_collapsed)
        printf("inv: inheritance OK\n");
    else
        printf("inv: INHERITANCE NOT DEMONSTRATED\n");
}
