# Native tests for the seq core. `make -C test` runs them.
CC      ?= cc
CFLAGS  ?= -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   := build

all: $(BUILD)/test_seq
	./$(BUILD)/test_seq

$(BUILD)/test_seq: test_seq.c ../core/seq.c ../core/seq.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../core -o $@ test_seq.c ../core/seq.c -lm

clean:
	rm -rf $(BUILD)

.PHONY: all clean
