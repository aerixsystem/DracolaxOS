# LXScript toolchain build (host-side)
CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c11

SRCS := lxscript.c lexer/lexer.c vm/vm.c stdlib/stdlib.c tools/lxs_cli.c
BIN  := draco-script

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f $(BIN)
