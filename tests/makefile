CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror
INCLUDES = -I../workspace/all/common
TEST_BIN ?= /tmp/nextui-fn-action-test

.PHONY: test test-fn test-text test-manifest test-i18n test-app-state clean

test: test-fn test-text test-manifest test-i18n test-app-state

test-fn:
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TEST_BIN) fn_action_test.c
	$(TEST_BIN)

test-text:
	../workspace/all/common/tests/run_text_layout_test.sh

test-manifest:
	../workspace/all/common/tests/run_pak_manifest_test.sh

test-i18n:
	./run_i18n_test.sh

test-app-state:
	../workspace/all/common/tests/run_app_state_test.sh

clean:
	rm -f $(TEST_BIN)
