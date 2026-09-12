/*
 * main.c - wiring and supervisor. app_main IS the supervisor: it creates
 * the plumbing and the three pipeline tasks, then settles into the
 * control-plane loop - shell, liveness audit, watchdog feed, heartbeat.
 *
 * Core map (one sentence per pin, as promised):
 *   core 1 - data plane: sampler + aggregator, so acquisition never
 *            contends with console/radio work.
 *   core 0 - control plane: supervisor + uplink, next to where Wi-Fi/BT
 *            would live.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "app_tasks.h"
#include "stacks.h"

static const char *TAG = "supervisor";

#define SUPERVISOR_PRIO   5
#define SUPERVISOR_LAP_MS 50
#define SHELL_LINE_MAX    32

EventGroupHandle_t   alive_group;
StreamBufferHandle_t sample_stream;
QueueHandle_t        uplink_queue;

TaskHandle_t sampler_handle;
TaskHandle_t aggregator_handle;
TaskHandle_t uplink_handle;

/* ------------------------------------------------------------------ hwm --
 * The stack audit. The firmware compares the numbers itself and only
 * prints "hwm: all margins OK" when every task still has at least
 * HWM_MARGIN_MIN_BYTES it has never touched - CI waits for that exact
 * line, because a scenario can match text but cannot do arithmetic
 * (03's `-> plausible` pattern). The failure string shares no substring
 * with the pass string.
 */
static void hwm_report(void)
{
    const struct {
        const char *name;
        uint32_t    declared;
    } rows[] = {
        { "sampler",    SAMPLER_STACK_BYTES },
        { "aggregator", AGGREGATOR_STACK_BYTES },
        { "uplink",     UPLINK_STACK_BYTES },
        { "supervisor", CONFIG_ESP_MAIN_TASK_STACK_SIZE },
    };
    /* NULL means "the calling task", i.e. the supervisor itself. */
    TaskHandle_t handles[] = { sampler_handle, aggregator_handle,
                               uplink_handle, NULL };

    bool all_ok = true;
    printf("hwm: task        declared  high-water  min-margin  verdict\n");
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        /* Bytes on this port (portSTACK_TYPE is uint8_t - see stacks.h). */
        uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(handles[i]);
        bool ok = hwm >= HWM_MARGIN_MIN_BYTES;
        all_ok = all_ok && ok;
        printf("hwm: %-11s %8lu  %10lu  %10u  %s\n",
               rows[i].name, (unsigned long)rows[i].declared,
               (unsigned long)hwm, HWM_MARGIN_MIN_BYTES,
               ok ? "ok" : "LOW");
    }
    if (all_ok)
        printf("hwm: all margins OK\n");
    else
        printf("hwm: MARGIN VIOLATION - a declared stack is nearly spent\n");
}

/* ---------------------------------------------------------------- shell -- */
static void shell_banner(void)
{
    printf("shell: commands: hwm | inv | crash\n> ");
    fflush(stdout);
}

static void dispatch(const char *line)
{
    if (strcmp(line, "hwm") == 0)        hwm_report();
    else if (strcmp(line, "inv") == 0)   inversion_run();
    else if (strcmp(line, "crash") == 0) crash_now();
    else if (line[0] != '\0')
        printf("unknown command '%s' - try hwm | inv | crash\n", line);
    printf("> ");
    fflush(stdout);
}

/* Polls UART0 without blocking the supervisor lap; echoes (the monitor
 * does not), dispatches on end-of-line. */
static void shell_poll(void)
{
    static char   line[SHELL_LINE_MAX];
    static size_t len;

    uint8_t buf[16];
    int n = uart_read_bytes(UART_NUM_0, buf, sizeof buf, 0);
    for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line[len] = '\0';
            len = 0;
            dispatch(line);
        } else if (len < sizeof line - 1) {
            putchar(c);
            line[len++] = c;
        }
    }
    if (n > 0) fflush(stdout);
}

/* ------------------------------------------------------------- app_main -- */
void app_main(void)
{
    /* The main task boots at priority 1; the supervisor must outrank the
     * pipeline it audits (and stay preemptible by the inv experiment). */
    vTaskPrioritySet(NULL, SUPERVISOR_PRIO);

    alive_group = xEventGroupCreate();

    /* Stream buffer, sampler -> aggregator. A stream buffer has NO
     * internal locking between multiple writers or multiple readers: it
     * is specified for exactly ONE writer task and ONE reader task, the
     * same ownership discipline that made project 02's SPSC ring buffer
     * lock-free. Sampler is the only writer, aggregator the only reader;
     * add a second of either and this line is where the design breaks.
     * Capacity: 1 s of samples; trigger level: one sampler lap. */
    sample_stream = xStreamBufferCreate(SAMPLE_RATE_HZ * 2,
                                        SAMPLES_PER_LAP * 2);

    /* Queue, aggregator -> uplink: copy-by-value structs, safe from any
     * number of contexts - which is why the RESULTS use a queue while the
     * byte stream above does not. 4 windows of headroom. */
    uplink_queue = xQueueCreate(4, sizeof(uplink_msg_t));

    configASSERT(alive_group && sample_stream && uplink_queue);

    /* UART0 driver for the shell's receive side (stdout keeps using the
     * console vfs); 256-byte RX ring, no TX buffer, no event queue. */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));

    /* The pipeline. Stack sizes and their margins: stacks.h. */
    xTaskCreatePinnedToCore(sampler_task, "sampler", SAMPLER_STACK_BYTES,
                            NULL, 4, &sampler_handle, 1);
    xTaskCreatePinnedToCore(aggregator_task, "aggregator",
                            AGGREGATOR_STACK_BYTES, NULL, 3,
                            &aggregator_handle, 1);
    xTaskCreatePinnedToCore(uplink_task, "uplink", UPLINK_STACK_BYTES,
                            NULL, 3, &uplink_handle, 0);

    /* The supervisor feeds the task watchdog for itself; the idle tasks
     * stay subscribed too (sdkconfig), so a spinning task on either core
     * still trips it - the CI scenarios deliberately outlive the 5 s
     * window to prove nobody starves. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    ESP_LOGI(TAG, "up: sampler c1/p4, aggregator c1/p3, uplink c0/p3, "
                  "supervisor c0/p%d", SUPERVISOR_PRIO);
    shell_banner();

    int32_t last_audit_s = 0, last_beat_s = 0;
    for (;;) {
        shell_poll();
        esp_task_wdt_reset();

        int32_t up_s = (int32_t)(esp_timer_get_time() / 1000000);

        /* Liveness audit, once a second: every pipeline task must have
         * set its bit since the last clear. */
        if (up_s != last_audit_s) {
            last_audit_s = up_s;
            EventBits_t bits = xEventGroupGetBits(alive_group);
            xEventGroupClearBits(alive_group, ALIVE_ALL);
            if ((bits & ALIVE_ALL) != ALIVE_ALL)
                ESP_LOGE(TAG, "task stalled: alive bits 0x%x, want 0x%x",
                         (unsigned)bits, (unsigned)ALIVE_ALL);
        }

        /* Heartbeat every 5 s - the CI scenarios wait for "alive t=10s"
         * so every run outlives the watchdog window (01's lesson). */
        if (up_s >= last_beat_s + 5) {
            last_beat_s = up_s - (up_s % 5);
            ESP_LOGI(TAG, "alive t=%lds", (long)last_beat_s);
        }

        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_LAP_MS));
    }
}
