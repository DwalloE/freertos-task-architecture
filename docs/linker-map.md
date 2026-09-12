# Where the bytes live — this build's linker map, read

Every number below comes from this repo's own CI builds — `idf.py size` /
`size-components` output printed in the "report image size" step, and
`build/freertos-task-architecture.map` shipped in the `firmware` artifact of every run —
so each figure is traceable to a run log. Sources: run 34688808625 ("before", commit
`c2c5976`, the aggregator's chunk on its stack) and run 34689007933 ("after", commit
`68f9775`, the chunk static). ESP-IDF v5.3, `-O2`, GCC 13.2.

## Section totals (after-state, the shipped configuration)

| Section | Bytes | Where it lives at runtime | What pays for it |
|---|---:|---|---|
| Flash `.text` | 84,560 | executes from flash via cache | code not marked IRAM |
| IRAM `.text` | 55,331 | internal RAM, always resident | ISRs, freertos hot paths, anything `IRAM_ATTR` |
| `.vectors` | 1,027 | IRAM | exception entry |
| `.rodata` | 42,468 | flash, cache-mapped | strings and const tables |
| `.data` | 8,692 | DRAM, **plus** a flash copy | initialized globals — every byte counts twice |
| `.bss` | 3,152 | DRAM only, zeroed at boot | uninitialized globals |
| Total image | 192,334 | flash | `.bss` is not in it (see below) |

## The ten biggest symbols, and why

From the `.map` (loadable sections only — `.debug_*` never leaves the ELF):

| Bytes | Symbol (section) | Why it is there |
|---:|---|---|
| 28,691 | `.rodata...init_show_app_info.str1.4` | the linker's **merged string pool**: all `-fmerge-constants` string sections fold into one, attributed to its first contributor — this is most of the firmware's log/format text, not one function's data |
| 13,444 | `vfprintf` (`.text`) | full newlib printf with float support |
| 13,053 | `svfprintf` | the same engine again, for `snprintf` |
| 9,480 | `vfiprintf` | and again, integer-only variant |
| 9,358 | `svfiprintf` | integer-only `snprintf` variant |
| 8,121 | `svfiscanf` | scanf engine, pulled in by the console vfs |
| 6,126 | `esp_err_to_name` strings (`.rodata`) | every `ESP_ERR_*` name, so `ESP_ERROR_CHECK` can print one |
| 3,505 | `dtoa` (`.text`) | float-to-decimal, a printf dependency |
| 2,816 | `mbedtls_sha256_software_process` | app image hashing at boot |
| 2,606 | `mprec` (`.text`) | arbitrary-precision support for dtoa |

The lesson in one line: **printf and its dependents are ~50 KB of this 192 KB image** —
four copies of the format engine plus dtoa/mprec — which is also why `uplink`, the one
task that formats output every second, declares the largest stack of the pipeline
(`main/stacks.h`). On a flash-tight part, `CONFIG_NEWLIB_NANO_FORMAT` is the first knob.

For scale, the whole application component (`libmain.a`) is 3,113 bytes: 2,492 flash
`.text`, 580 `.bss`, 41 `.rodata`. Everything else is the platform.

## The experiment: one buffer, two homes

The aggregator drains the stream buffer through a 512-byte chunk. Through commit
`c2c5976` it lived on the task's stack; commit `68f9775` made it
`static uint8_t chunk[512]`. Same bytes, different home — measured consequences:

|  | chunk on the task stack (before) | `static` chunk (after) |
|---|---:|---:|
| app `.bss` (idf.py size) | 2,640 | 3,152 (**+512, exactly**) |
| `libmain.a` `.bss` (size-components) | 68 | 580 |
| the symbol in the `.map` | — (anonymous stack bytes) | `.bss.chunk$0  0x3ffb2908  0x200  aggregator.c.obj` |
| aggregator declared stack (`stacks.h`) | 3,328 | 2,816 (**−512**) |
| `.text` / `.data` / `.rodata` | unchanged | unchanged |
| total image size | 192,334 | 192,334 (**unchanged**) |

Three things this table says that hand-waving does not:

1. **`.bss` is free in flash and paid in RAM.** The image size did not move a byte:
   `.bss` is only a size and an address; startup zeroes it. (Had the buffer been
   *initialized*, it would have landed in `.data` and cost 512 bytes of flash **and**
   512 of RAM.)
2. **Total RAM is conserved, visibility is not.** Stack demand fell by the same 512 the
   `.bss` grew — but the static byte is a *named, linker-visible, permanent* allocation,
   while the stack byte was anonymous, existed only while the function ran, and was
   invisible to everything except the high-water mark. On this always-running task the
   buffer is live forever, so making it static costs nothing extra and lets the declared
   stack shrink by a full 512.
3. **The trade has a safety edge.** A stack overflow is caught here by the canary (the
   `crash` demo); a static buffer overrun scribbles on whatever the linker placed next —
   check the neighbours of `0x3ffb2908` in the map. And a static buffer is only legal
   because the aggregator is the stream's **single reader**; on the stack, re-entrancy
   was structurally free.

The stack side of the ledger is not taken on faith: the `hwm` shell command re-measures
every task's high-water mark, and CI's healthy scenario requires the firmware's own
`hwm: all margins OK` verdict on every run. Measured in the shipped configuration (run
34691078128, `wokwi-serial-healthy.log`):

```text
hwm: task        declared  high-water  min-margin  verdict
hwm: sampler         2560        2044         512  ok
hwm: aggregator      2816        2248         512  ok
hwm: uplink          3072        2532         512  ok
hwm: supervisor      3584        2184         512  ok
hwm: all margins OK
```

The aggregator keeps 2,248 bytes of headroom at the reduced 2,816 declaration — the
512-byte cut came out of bytes the task, minus its buffer, never touched.

## Honest limits

These numbers are for **this build**: IDF v5.3, `-O2`, this sdkconfig. `-Os` moves every
`.text` figure; a different IDF minor moves the platform's share; enabling Wi-Fi would
dwarf all of it (the radio stacks add ~100 KB of IRAM/DRAM pressure). The method — read
the map, name the ten biggest, know which section each byte bills to — is the part that
transfers.
