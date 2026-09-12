/*
 * app_tasks.h - the plumbing between the four tasks. Who owns what:
 *
 *   sampler ──stream buffer──▶ aggregator ──queue──▶ uplink
 *                 (bytes)                 (structs)
 *   supervisor: owns the event group, the watchdog feed, and the shell.
 *
 * The stream buffer carries the bulk byte stream; the queue carries
 * fixed-size results. That split is the design point: a stream buffer is
 * cheaper than a queue per byte but is restricted to ONE writer and ONE
 * reader (see the creation site in main.c), a queue is copy-by-value and
 * safe from anywhere.
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"

#include "agg_core.h"

/* Liveness bits: each pipeline task sets its bit every lap; the
 * supervisor clears and re-checks them once a second. */
#define ALIVE_SAMPLER     (1u << 0)
#define ALIVE_AGGREGATOR  (1u << 1)
#define ALIVE_UPLINK      (1u << 2)
#define ALIVE_ALL         (ALIVE_SAMPLER | ALIVE_AGGREGATOR | ALIVE_UPLINK)

/* 100 Hz synthetic sampling: 4 samples per 40 ms lap (a 40 ms lap is 4
 * ticks at the 100 Hz tick - never 1 ms, which truncates to 0 ticks). */
#define SAMPLE_RATE_HZ        100
#define SAMPLES_PER_LAP       4
#define SAMPLER_LAP_MS        40
#define AGG_WINDOW_SAMPLES    100   /* one result per second */

/* What crosses the aggregator->uplink queue. */
typedef struct {
    uint32_t     seq;
    agg_result_t agg;
} uplink_msg_t;

/* Created in main.c before any task starts. */
extern EventGroupHandle_t   alive_group;
extern StreamBufferHandle_t sample_stream;
extern QueueHandle_t        uplink_queue;

/* Task handles, for the hwm audit. */
extern TaskHandle_t sampler_handle;
extern TaskHandle_t aggregator_handle;
extern TaskHandle_t uplink_handle;

void sampler_task(void *arg);
void aggregator_task(void *arg);
void uplink_task(void *arg);

/* Shell commands, implemented in their own files. */
void inversion_run(void);   /* inversion.c: prints figures + verdict line */
void crash_now(void);       /* crash.c: spawns the victim, does not return a result */

#endif /* APP_TASKS_H */
