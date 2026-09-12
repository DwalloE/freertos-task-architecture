/*
 * aggregator.c - drains the sample stream, folds 1 s windows, ships
 * results to the uplink queue. Pinned to core 1 with the sampler: the
 * data plane stays on one core so the stream buffer's producer/consumer
 * handoff never crosses cores (cheaper wakeups, and the pairing makes
 * the single-reader contract easy to audit).
 */
#include "app_tasks.h"

#include "esp_log.h"

static const char *TAG = "aggregator";

static uint32_t windows_dropped;

void aggregator_task(void *arg)
{
    (void)arg;

    /* Receive chunk lives on the task stack for now. The linker-map
     * experiment (docs/linker-map.md) moves it to static storage and
     * re-measures both .bss and this task's high-water mark - this
     * buffer is the "one buffer" that doc talks about. */
    uint8_t chunk[512];

    /* A sample is 2 bytes; a stream buffer hands back BYTES with no
     * message framing, so a read may split a sample. Carry the odd byte. */
    uint8_t  pending_byte = 0;
    bool     have_pending = false;

    agg_window_t w;
    agg_reset(&w);
    uint32_t seq = 0;

    for (;;) {
        /* Sole reader of sample_stream (contract at the creation site). */
        size_t got = xStreamBufferReceive(sample_stream, chunk, sizeof chunk,
                                          pdMS_TO_TICKS(100));

        size_t i = 0;
        while (i < got) {
            int16_t s;
            if (have_pending) {
                uint8_t pair[2] = { pending_byte, chunk[i++] };
                s = agg_decode_sample(pair);
                have_pending = false;
            } else if (got - i >= 2) {
                s = agg_decode_sample(&chunk[i]);
                i += 2;
            } else {
                pending_byte = chunk[i++];
                have_pending = true;
                break;
            }

            agg_add(&w, s);
            if (w.count >= AGG_WINDOW_SAMPLES) {
                uplink_msg_t msg = { .seq = seq++ };
                if (agg_finalize(&w, &msg.agg)) {
                    /* Short timeout, then drop-and-count: the aggregator
                     * must never let a slow uplink back up into the
                     * sample stream. */
                    if (xQueueSend(uplink_queue, &msg,
                                   pdMS_TO_TICKS(20)) != pdTRUE) {
                        windows_dropped++;
                        ESP_LOGW(TAG, "uplink queue full, %lu windows dropped",
                                 (unsigned long)windows_dropped);
                    }
                }
                agg_reset(&w);
            }
        }

        xEventGroupSetBits(alive_group, ALIVE_AGGREGATOR);
    }
}
