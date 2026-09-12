# agg_core.h is IDF-free on purpose: test/Makefile compiles the same
# header with plain cc, so the fold's branches are host-gated before they
# run under the scheduler.
idf_component_register(
    SRCS "main.c" "sampler.c" "aggregator.c" "uplink.c"
         "inversion.c" "crash.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_timer log
)
