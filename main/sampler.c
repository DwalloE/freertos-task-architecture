/*
 * sampler.c - the synthetic "ADC front end". Pinned to core 1: on this
 * design core 1 is the data-plane core (acquisition + aggregation stay
 * cache-warm together and never contend with the radio/console work on
 * core 0), which is the same split ESP-IDF itself uses when it parks
 * Wi-Fi/BT on core 0.
 */
#include "app_tasks.h"

#include "esp_log.h"

static const char *TAG = "sampler";

static uint32_t samples_dropped;

void sampler_task(void *arg)
{
    (void)arg;
    uint32_t n = 0;

    for (;;) {
        uint8_t chunk[SAMPLES_PER_LAP * 2];
        for (int i = 0; i < SAMPLES_PER_LAP; i++)
            agg_encode_sample(agg_synth_sample(n++), &chunk[i * 2]);

        /* Sole writer of sample_stream - the other half of the contract
         * stated at its creation site in main.c. Timeout 0: an ADC does
         * not wait for its consumer; if the stream is full the samples
         * are dropped and counted, exactly like a hardware FIFO overrun. */
        size_t sent = xStreamBufferSend(sample_stream, chunk, sizeof chunk, 0);
        if (sent < sizeof chunk) {
            samples_dropped += (uint32_t)(sizeof chunk - sent) / 2;
            ESP_LOGW(TAG, "stream full, %lu samples dropped so far",
                     (unsigned long)samples_dropped);
        }

        xEventGroupSetBits(alive_group, ALIVE_SAMPLER);
        vTaskDelay(pdMS_TO_TICKS(SAMPLER_LAP_MS));
    }
}
