/*
 * browser-demo/sketch.ino - Arduino-core port of freertos-task-architecture,
 * for the shareable Wokwi project. A DEMO, not the firmware: browser Wokwi
 * cannot compile ESP-IDF, so this file re-states the four-task architecture
 * on the Arduino core, whose FreeRTOS exposes the same tasks, queues, event
 * groups, stream buffers and both lock types.
 *
 * Honest differences from main/:
 *  1. The supervisor here is Arduino's loopTask (stack sized by the core,
 *     not by sdkconfig), and there is no esp_task_wdt feed - the Arduino
 *     core manages the watchdog its own way. The liveness audit still runs.
 *  2. The aggregation arithmetic is inlined below; main/agg_core.h is the
 *     host-tested original. If the two ever disagree, main/ is right.
 *  3. `crash` relies on the core's stack-overflow canary; the panic text
 *     can differ from the ESP-IDF v5.3 build that CI asserts.
 *
 * Type into the serial monitor:  hwm   inv   crash
 */
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

/* ---------------------------------------------------- aggregation core -- */
typedef struct { int32_t sum; int16_t min, max; uint16_t count; } agg_window_t;
typedef struct { int16_t min, max, mean; uint16_t count; } agg_result_t;

static void agg_reset(agg_window_t *w)
{
  w->sum = 0; w->count = 0; w->min = INT16_MAX; w->max = INT16_MIN;
}
static void agg_add(agg_window_t *w, int16_t s)
{
  w->sum += s;
  if (s < w->min) w->min = s;
  if (s > w->max) w->max = s;
  w->count++;
}
static bool agg_finalize(const agg_window_t *w, agg_result_t *out)
{
  if (w->count == 0) return false;
  int32_t half = (int32_t)w->count / 2;
  out->min = w->min; out->max = w->max; out->count = w->count;
  out->mean = (int16_t)((w->sum >= 0) ? (w->sum + half) / w->count
                                      : (w->sum - half) / w->count);
  return true;
}
static int16_t synth_sample(uint32_t n)
{
  uint32_t phase = n % 200;
  int32_t tri = (phase < 100) ? ((int32_t)phase * 20 - 1000)
                              : (1000 - (int32_t)(phase - 100) * 20);
  return (int16_t)(tri + (int32_t)((n * 31u) % 7u) - 3);
}

/* -------------------------------------------------------------- plumbing -- */
#define ALIVE_SAMPLER (1u << 0)
#define ALIVE_AGG     (1u << 1)
#define ALIVE_UPLINK  (1u << 2)
#define ALIVE_ALL     0x7u

typedef struct { uint32_t seq; agg_result_t agg; } uplink_msg_t;

static EventGroupHandle_t   aliveGroup;
static StreamBufferHandle_t sampleStream;   /* ONE writer, ONE reader */
static QueueHandle_t        uplinkQueue;
static TaskHandle_t hSampler, hAgg, hUplink;

#define SAMPLER_STACK 2560
#define AGG_STACK     2816
#define UPLINK_STACK  3072
#define MARGIN_MIN    512

/* ------------------------------------------------------------ the tasks -- */
static void samplerTask(void *)
{
  uint32_t n = 0;
  for (;;) {
    int16_t s[4];
    for (int i = 0; i < 4; i++) s[i] = synth_sample(n++);
    xStreamBufferSend(sampleStream, s, sizeof s, 0);  /* ADCs don't wait */
    xEventGroupSetBits(aliveGroup, ALIVE_SAMPLER);
    vTaskDelay(pdMS_TO_TICKS(40));                    /* 100 Hz overall */
  }
}

static void aggTask(void *)
{
  int16_t chunk[128];
  agg_window_t w; agg_reset(&w);
  uint32_t seq = 0;
  for (;;) {
    size_t got = xStreamBufferReceive(sampleStream, chunk, sizeof chunk,
                                      pdMS_TO_TICKS(100));
    for (size_t i = 0; i < got / 2; i++) {
      agg_add(&w, chunk[i]);
      if (w.count >= 100) {                            /* 1 s window */
        uplink_msg_t msg = { seq++ };
        if (agg_finalize(&w, &msg.agg))
          xQueueSend(uplinkQueue, &msg, pdMS_TO_TICKS(20));
        agg_reset(&w);
      }
    }
    xEventGroupSetBits(aliveGroup, ALIVE_AGG);
  }
}

static void uplinkTask(void *)
{
  uplink_msg_t msg;
  for (;;) {
    if (xQueueReceive(uplinkQueue, &msg, pdMS_TO_TICKS(500)) == pdTRUE)
      Serial.printf("uplink: seq=%lu n=%u min=%d max=%d mean=%d\n",
                    (unsigned long)msg.seq, msg.agg.count,
                    msg.agg.min, msg.agg.max, msg.agg.mean);
    xEventGroupSetBits(aliveGroup, ALIVE_UPLINK);
  }
}

/* ------------------------------------------------------------------ hwm -- */
static void hwmReport()
{
  struct { const char *name; TaskHandle_t h; uint32_t declared; } rows[] = {
    { "sampler",    hSampler, SAMPLER_STACK },
    { "aggregator", hAgg,     AGG_STACK },
    { "uplink",     hUplink,  UPLINK_STACK },
    { "supervisor", NULL,     8192 },   /* Arduino's loopTask default */
  };
  bool allOk = true;
  Serial.println("hwm: task        declared  high-water  min-margin  verdict");
  for (auto &r : rows) {
    uint32_t hwm = uxTaskGetStackHighWaterMark(r.h);   /* bytes on ESP32 */
    bool ok = hwm >= MARGIN_MIN;
    allOk = allOk && ok;
    Serial.printf("hwm: %-11s %8lu  %10lu  %10u  %s\n", r.name,
                  (unsigned long)r.declared, (unsigned long)hwm,
                  MARGIN_MIN, ok ? "ok" : "LOW");
  }
  Serial.println(allOk ? "hwm: all margins OK"
                       : "hwm: MARGIN VIOLATION - a declared stack is nearly spent");
}

/* ------------------------------------------------- priority inversion ---- */
typedef struct {
  SemaphoreHandle_t lock, done;
  volatile bool lowHasLock;
  volatile int64_t blockedUs;
} inv_ctx_t;

static void busyMs(int32_t ms)
{
  int64_t tEnd = esp_timer_get_time() + (int64_t)ms * 1000;
  volatile uint32_t sink = 0;
  while (esp_timer_get_time() < tEnd) sink++;
}
static void invLow(void *a)
{
  inv_ctx_t *c = (inv_ctx_t *)a;
  xSemaphoreTake(c->lock, portMAX_DELAY);
  c->lowHasLock = true;
  busyMs(50);
  xSemaphoreGive(c->lock);
  xSemaphoreGive(c->done);
  vTaskDelete(NULL);
}
static void invHigh(void *a)
{
  inv_ctx_t *c = (inv_ctx_t *)a;
  int64_t t0 = esp_timer_get_time();
  xSemaphoreTake(c->lock, portMAX_DELAY);
  c->blockedUs = esp_timer_get_time() - t0;
  xSemaphoreGive(c->lock);
  xSemaphoreGive(c->done);
  vTaskDelete(NULL);
}
static void invMid(void *a)
{
  inv_ctx_t *c = (inv_ctx_t *)a;
  busyMs(300);
  xSemaphoreGive(c->done);
  vTaskDelete(NULL);
}

static int64_t invRound(bool useMutex)
{
  inv_ctx_t ctx = {};
  if (useMutex) ctx.lock = xSemaphoreCreateMutex();
  else { ctx.lock = xSemaphoreCreateBinary(); xSemaphoreGive(ctx.lock); }
  ctx.done = xSemaphoreCreateCounting(3, 0);

  /* All on ONE core, on purpose: a migratable task dodges the inversion.
   * Core 0 here, NOT core 1 as in main/: the Arduino core runs loop() -
   * this orchestrator - on core 1 at priority 1, and putting the
   * participants beside it starves the orchestrator during low's hold,
   * so high gets created only after the lock is already free (measured:
   * 9 us blocked on both rounds - see the README bug gallery). */
  xTaskCreatePinnedToCore(invLow, "inv_low", 2560, &ctx, 5, NULL, 0);
  while (!ctx.lowHasLock) vTaskDelay(pdMS_TO_TICKS(10));
  xTaskCreatePinnedToCore(invHigh, "inv_high", 2560, &ctx, 8, NULL, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  xTaskCreatePinnedToCore(invMid, "inv_mid", 2560, &ctx, 6, NULL, 0);

  bool ok = true;
  for (int i = 0; i < 3; i++)
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(2000)) != pdTRUE) ok = false;
  vSemaphoreDelete(ctx.lock);
  vSemaphoreDelete(ctx.done);
  return ok ? ctx.blockedUs : -1;
}

static void inversionRun()
{
  Serial.println("inv: lock held 50 ms by prio-5 task; prio-6 spinner runs "
                 "300 ms; prio-8 waiter measured; all on core 0");
  int64_t semUs = invRound(false);
  int64_t mutUs = invRound(true);
  if (semUs < 0 || mutUs < 0) { Serial.println("inv: EXPERIMENT WEDGED"); return; }
  Serial.printf("inv: binary semaphore (no inheritance): high-prio blocked %lld us\n",
                (long long)semUs);
  Serial.printf("inv: mutex (priority inheritance):      high-prio blocked %lld us\n",
                (long long)mutUs);
  Serial.printf("inv: ratio %lld.%lldx\n", (long long)(semUs / mutUs),
                (long long)((semUs * 10 / mutUs) % 10));
  bool spike = semUs > 150000, collapsed = mutUs < 120000 && mutUs * 3 < semUs;
  Serial.println(spike && collapsed ? "inv: inheritance OK"
                                    : "inv: INHERITANCE NOT DEMONSTRATED");
}

/* ---------------------------------------------------------------- crash -- */
static void __attribute__((noinline)) plunge(void)
{
  /* One frame bigger than any headroom eatStack leaves, so the write
   * provably crosses the canary; the next context switch panics. */
  volatile uint8_t last[512];
  for (unsigned i = 0; i < sizeof last; i++) last[i] = 0xEE;
  for (;;) vTaskDelay(pdMS_TO_TICKS(50));
}
static void __attribute__((noinline)) eatStack(uint32_t depth)
{
  volatile uint8_t frame[128];
  for (unsigned i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)depth;
  if (uxTaskGetStackHighWaterMark(NULL) > sizeof frame + 96) eatStack(depth + 1);
  else plunge();                     /* walk to the edge, step over it */
}
static void victimTask(void *)
{
  Serial.println("crash: victim task up with 2048 bytes of stack, recursing "
                 "past the end on purpose");
  eatStack(1);
}
static void crashNow()
{
  xTaskCreatePinnedToCore(victimTask, "victim", 2048, NULL, 4, NULL, 1);
}

/* ----------------------------------------------------------- supervisor -- */
void setup()
{
  Serial.begin(115200);
  aliveGroup   = xEventGroupCreate();
  sampleStream = xStreamBufferCreate(200, 8);   /* ONE writer, ONE reader */
  uplinkQueue  = xQueueCreate(4, sizeof(uplink_msg_t));

  /* core 1 = data plane, core 0 = control plane (loopTask lives on 1 in
   * the Arduino core - the pin CHOICES are the demo, see main/ comments) */
  xTaskCreatePinnedToCore(samplerTask, "sampler", SAMPLER_STACK, NULL, 4, &hSampler, 1);
  xTaskCreatePinnedToCore(aggTask, "aggregator", AGG_STACK, NULL, 3, &hAgg, 1);
  xTaskCreatePinnedToCore(uplinkTask, "uplink", UPLINK_STACK, NULL, 3, &hUplink, 0);

  Serial.println("shell: commands: hwm | inv | crash");
  Serial.print("> ");
}

static char line[32];
static size_t lineLen = 0;
static int32_t lastAuditS = 0, lastBeatS = 0;

void loop()
{
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      Serial.println();
      line[lineLen] = '\0'; lineLen = 0;
      if (!strcmp(line, "hwm")) hwmReport();
      else if (!strcmp(line, "inv")) inversionRun();
      else if (!strcmp(line, "crash")) crashNow();
      else if (line[0]) Serial.printf("unknown command '%s' - try hwm | inv | crash\n", line);
      Serial.print("> ");
    } else if (lineLen < sizeof line - 1) {
      Serial.print(c);                          /* echo: the monitor doesn't */
      line[lineLen++] = c;
    }
  }

  int32_t upS = (int32_t)(esp_timer_get_time() / 1000000);
  if (upS != lastAuditS) {
    lastAuditS = upS;
    EventBits_t bits = xEventGroupGetBits(aliveGroup);
    xEventGroupClearBits(aliveGroup, ALIVE_ALL);
    if ((bits & ALIVE_ALL) != ALIVE_ALL)
      Serial.printf("supervisor: task stalled: alive bits 0x%x, want 0x7\n",
                    (unsigned)bits);
  }
  if (upS >= lastBeatS + 5) {
    lastBeatS = upS - (upS % 5);
    Serial.printf("supervisor: alive t=%lds\n", (long)lastBeatS);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}
