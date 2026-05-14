CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lpthread -lm

LIB_SRCS := src/worker_node.c src/control_node.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
LIB      := libcluster.a

WORKER_BIN  := worker_app
CONTROL_BIN := control_app

.PHONY: all clean run test docs

all: $(WORKER_BIN) $(CONTROL_BIN)

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(WORKER_BIN): examples/worker_app.c $(LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(CONTROL_BIN): examples/control_app.c $(LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Quick single-machine demo:
#   worker 1 – port 9001, 2 cores, 60 s timeout
#   worker 2 – port 9002, 2 cores, 60 s timeout
#   control  – requires 2 workers, 60 s timeout, ε = 1e-9
run: all
	@echo "=== Starting workers in background ==="
	./$(WORKER_BIN) 9001 2 60 &
	./$(WORKER_BIN) 9002 2 60 &
	sleep 0.2
	@echo "=== Starting control ==="
	./$(CONTROL_BIN) 2 60 127.0.0.1:9001 127.0.0.1:9002

test: all
	chmod +x test.sh
	./test.sh

docs:
	doxygen Doxyfile

clean:
	rm -f $(LIB_OBJS) $(LIB) $(WORKER_BIN) $(CONTROL_BIN)
	rm -rf docs
