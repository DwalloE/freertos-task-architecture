/*
 * stacks.h - every task's declared stack, in one place, so the sizing
 * arithmetic is quotable instead of scattered. The rule used throughout:
 *
 *     declared = measured high-water demand (docs/linker-map.md, from a
 *                real -O2 build) + HWM_MARGIN_MIN_BYTES of declared
 *                headroom, rounded up to 256.
 *
 * The margin is not decoration: the `hwm` shell command re-measures every
 * task with uxTaskGetStackHighWaterMark() and the firmware itself refuses
 * to print "hwm: all margins OK" unless every task still clears
 * HWM_MARGIN_MIN_BYTES. CI runs that command and waits for that line, so
 * a change that eats a task's headroom fails the build, not the field.
 *
 * ESP-IDF note: portSTACK_TYPE is uint8_t on this port, so both the
 * xTaskCreatePinnedToCore() depth argument and the high-water return
 * value are in BYTES (vanilla FreeRTOS counts words - a 4x trap when
 * porting these numbers anywhere else; docs/mapping-to-zephyr.md).
 */
#ifndef STACKS_H
#define STACKS_H

/* Minimum headroom (bytes never touched) every task must keep. */
#define HWM_MARGIN_MIN_BYTES  512

/* sampler: synthesis + a 8-byte encode chunk, no printf on its hot path */
#define SAMPLER_STACK_BYTES     2560

/* aggregator: the fold; its 512-byte receive chunk moved to .bss (see
 * docs/linker-map.md for the before/after), so the stack shrank with it */
#define AGGREGATOR_STACK_BYTES  2816

/* uplink: one printf per window - printf is the stack hog here */
#define UPLINK_STACK_BYTES      3072

/* supervisor runs in the main task; its size is CONFIG_ESP_MAIN_TASK_STACK_SIZE
 * (sdkconfig, default 3584) - declared there, audited here like the rest */

/* inversion experiment participants (short-lived, see inversion.c) */
#define INV_TASK_STACK_BYTES    2560

/* the crash demo's victim: deliberately small - it exists to overflow */
#define VICTIM_STACK_BYTES      2048

#endif /* STACKS_H */
