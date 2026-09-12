# Host-native tests for main/agg_core.h. Needs only cc and make; ~1 s.
#
#   make          - functional run, then the branch-coverage gate
#
# No ThreadSanitizer stage here on purpose (03's rule against cargo-culting
# 02's gates): the concurrency in this project lives in FreeRTOS queues, a
# stream buffer and an event group, whose ordering the RTOS owns - a host
# TSan run over agg_core.h would test nothing. The pure window arithmetic
# IS gateable, so it gets 02's 100%-branch gate; the RTOS wiring is proven
# on target, over serial, by the CI scenarios.
#
# On macOS the gcov stanza format matched Apple's tools once the
# instrumented object is compiled inside build/ (02's lesson), so this
# should pass locally too. If it ever doesn't, the Linux CI run is the
# authoritative check.

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

BUILD := build

.PHONY: all run functional coverage clean

all: run

run: functional coverage
	@echo
	@echo "ALL TEST STAGES PASS"

$(BUILD):
	@mkdir -p $(BUILD)

# ---------------------------------------------------------------- functional
$(BUILD)/test_agg: test_agg.c ../main/agg_core.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_agg.c

functional: $(BUILD)/test_agg
	@echo "--- functional: fold edges, rounding, extremes, wire layout, synth"
	./$(BUILD)/test_agg

# ------------------------------------------------------------------- coverage
# --coverage at -O0 so gcov's branch view matches the source. The gate is
# Taken-at-least-once == 100.00% for every branch in agg_core.h.
coverage: | $(BUILD)
	@echo "--- coverage: every branch in agg_core.h must be taken both ways"
	cd $(BUILD) \
	    && $(CC) -O0 --coverage -std=c11 -Wall -Wextra -Werror \
	           -c ../test_agg.c -o cov_agg.o \
	    && $(CC) --coverage cov_agg.o -o cov_test \
	    && ./cov_test > /dev/null \
	    && gcov -b cov_agg.gcda > gcov_summary.txt
	@awk -v RS='' '/agg_core\.h/ { found = 1;                                 \
	        if ($$0 ~ /Taken at least once:100\.00%/) ok = 1;                 \
	        print }                                                           \
	    END {                                                                 \
	        if (!found) { print "no gcov stanza for agg_core.h - if this is"; \
	            print "macOS gcov formatting, the Linux CI run is the";       \
	            print "authoritative check"; exit 2 }                         \
	        exit ok ? 0 : 1 }' $(BUILD)/gcov_summary.txt

clean:
	rm -rf $(BUILD) *.gcno *.gcda *.gcov
