/*
 * uplink.c - drains the result queue and "transmits" (prints). Pinned to
 * core 0, the control-plane core: a real uplink is a radio, and on ESP32
 * the Wi-Fi/BT stacks live on core 0 - putting the transmitter next to
 * them now means the data plane on core 1 keeps its timing when the
 * printf below becomes an esp_wifi call later.
 */
#include <stdio.h>

#include "app_tasks.h"

void uplink_task(void *arg)
{
    (void)arg;
    uplink_msg_t msg;

    for (;;) {
        /* Finite timeout so the alive bit keeps being set (and the
         * supervisor can tell "no traffic" from "dead task") even when
         * the aggregator has nothing for us. */
        if (xQueueReceive(uplink_queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
            printf("uplink: seq=%lu n=%u min=%d max=%d mean=%d\n",
                   (unsigned long)msg.seq, msg.agg.count,
                   msg.agg.min, msg.agg.max, msg.agg.mean);
        }
        xEventGroupSetBits(alive_group, ALIVE_UPLINK);
    }
}
