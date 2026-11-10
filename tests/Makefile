CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -I../include
SANITIZE ?= 0

ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address,undefined -g -O1
LDFLAGS += -fsanitize=address,undefined
endif

UNIT_DIR = unit

.PHONY: all unit clean

all: unit

unit: $(UNIT_DIR)/test_sizevarint $(UNIT_DIR)/test_worldgen
	@echo "Running unit tests..."
	$(UNIT_DIR)/test_sizevarint
	$(UNIT_DIR)/test_worldgen

$(UNIT_DIR)/test_sizevarint: $(UNIT_DIR)/test_sizevarint.c
	$(CC) $(CFLAGS) -I$(UNIT_DIR) $< ../src/varnum.c -o $@ $(LDFLAGS) -lm

$(UNIT_DIR)/test_worldgen: $(UNIT_DIR)/test_worldgen.c $(UNIT_DIR)/stubs.c
	$(CC) $(CFLAGS) -I$(UNIT_DIR) $(UNIT_DIR)/test_worldgen.c $(UNIT_DIR)/stubs.c ../src/worldgen.c ../src/globals.c -o $@ $(LDFLAGS) -lm

clean:
	rm -f $(UNIT_DIR)/test_sizevarint $(UNIT_DIR)/test_worldgen
