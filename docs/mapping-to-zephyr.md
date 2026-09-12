# The same architecture in Zephyr and ThreadX vocabulary

Interviewers ask you to translate — "you did this in FreeRTOS, how would it look in
Zephyr?" — so here is this repo's exact structure, primitive by primitive, in all three
vocabularies. Project 09 (`zephyr-nrf52840-sensor-node`) builds on the Zephyr column.

## The primitives this firmware uses

| This repo (FreeRTOS/ESP-IDF) | Zephyr | ThreadX | Notes that matter |
|---|---|---|---|
| `xTaskCreatePinnedToCore()` | `k_thread_create()` + `k_thread_cpu_pin()` | `tx_thread_create()`; SMP builds add `tx_thread_smp_core_exclude()` | Zephyr and ThreadX take a caller-supplied stack area (`K_THREAD_STACK_DEFINE`); FreeRTOS allocates from its heap unless you use the `Static` variant. So in Zephyr the linker-map story below is *visible at build time*: every thread stack is a named symbol in `.noinit`/`.bss`. |
| task priority (bigger = more important, 0 = idle) | thread priority (**smaller = more important**, negative = cooperative) | priority (**smaller = more important**, 0 highest) | The inversion demo's "low/middle/high" numbering flips in both other systems. Translate the *relationship*, never the numbers. |
| `xQueueCreate()` / `xQueueSend()` / `xQueueReceive()` | `k_msgq` (`K_MSGQ_DEFINE`, `k_msgq_put/get`) | `tx_queue_create/send/receive` | All three copy fixed-size messages by value. Zephyr's buffer is again a symbol you declare, not heap. |
| `xEventGroupCreate()` / wait bits / set bits | `k_event` (`k_event_post`, `k_event_wait_all`) | `tx_event_flags_create/get/set` | Same wait-any/wait-all/clear-on-exit semantics everywhere; Zephyr got `k_event` only in 3.x — older code (ab)used `k_poll`. |
| `xStreamBufferCreate()` (one writer, one reader) | `k_pipe` | `tx_byte_pool` feeding a `tx_queue`, or roll your own | The single-writer/single-reader restriction is FreeRTOS-specific: `k_pipe` takes multiple writers (it locks). The *reason* FreeRTOS's is restricted — no internal lock, same discipline as project 02's SPSC ring — is the interview answer. |
| `xSemaphoreCreateMutex()` (priority inheritance) | `k_mutex` (inheritance always on) | `tx_mutex_create(..., TX_INHERIT)` | Zephyr mutexes always inherit; ThreadX makes inheritance an explicit flag — the `inv` demo's binary-semaphore round is exactly what `TX_NO_INHERIT` buys you. Zephyr forbids mutex use from ISRs outright. |
| `xSemaphoreCreateBinary()` | `k_sem` (max count 1) | `tx_semaphore_create` | No inheritance in any of the three — semaphores signal, mutexes protect. Using one where the other belongs is the whole `inv` demo. |
| `uxTaskGetStackHighWaterMark()` | `k_thread_stack_space_get()` (needs `CONFIG_INIT_STACKS` + `CONFIG_THREAD_STACK_INFO`) | `tx_thread_info_get()` + `TX_ENABLE_STACK_CHECKING` | All three paint the stack and scan for the watermark. Units trap: this ESP-IDF port returns **bytes** (`portSTACK_TYPE` is `uint8_t`); vanilla FreeRTOS returns **words**; Zephyr returns **unused bytes** via an out-param with error return. |
| `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY` | `CONFIG_STACK_SENTINEL` (software canary) or `CONFIG_HW_STACK_PROTECTION` (MPU/MMU) | `tx_thread_stack_error_notify()` callback | Zephyr on Cortex-M with an MPU catches the overflow *on the faulting write*, not at the next context switch — strictly stronger than this repo's canary, worth saying in the interview. |
| task watchdog (`esp_task_wdt_*`) | `wdt` driver API + `CONFIG_TASK_WDT` (Zephyr 3.5+) or a supervisor thread | application-level, typically a supervisor thread + `tx_timer` | The supervisor-owns-the-feed pattern in `main.c` ports unchanged: one high-priority thread checks liveness bits, and only it strokes the hardware dog. |
| `esp_timer_get_time()` (64-bit µs) | `k_uptime_get()` (ms) / `k_cycle_get_64()` | `tx_time_get()` (ticks) | The inversion measurements need µs; in Zephyr use cycle counts and `k_cyc_to_us_floor64()`. |

## What does NOT translate

- **Core affinity as an afterthought.** ESP-IDF is FreeRTOS-with-SMP where affinity is a
  per-task creation argument. Zephyr SMP defaults to free migration and pinning is the
  exception (`k_thread_cpu_pin`); ThreadX SMP thinks in exclusion masks. The *decision*
  (data plane on one core, control plane on the other) carries over; the mechanism differs.
- **`app_main` as a task.** Zephyr's `main()` is a thread you configure via Kconfig
  (`CONFIG_MAIN_STACK_SIZE`, `CONFIG_MAIN_THREAD_PRIORITY`), which replaces this repo's
  `vTaskPrioritySet(NULL, ...)` boot dance with build-time declarations.
- **The stream buffer's trigger level** (bytes before the reader wakes) has no `k_pipe`
  equivalent; you get it back by sizing the reader's minimum read instead.

## Why this doc exists

The declared-stack table in `main/stacks.h`, the HWM audit, and the linker-map analysis
(`docs/linker-map.md`) are all *portable habits* — only the API names change. This file is
the dictionary; project 09 is the same architecture actually rebuilt on the Zephyr column.
