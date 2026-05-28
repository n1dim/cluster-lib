CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lpthread -lm

# Флаги безопасности (ГОСТ-Р 71206-2024, Приложение А)
CFLAGS_SEC := -O2 \
              -fstack-protector-strong \
              -D_FORTIFY_SOURCE=2 \
              -Wformat -Wformat-security \
              -fwrapv \
              -fno-delete-null-pointer-checks \
              -fPIE

LDFLAGS_SEC := -pie \
               -Wl,-z,relro \
               -Wl,-z,now

LIB_SRCS := src/worker_node.c src/control_node.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
LIB      := libcluster.a

WORKER_BIN  := worker_app
CONTROL_BIN := control_app

.PHONY: all clean run test docs bench analyze all-gcc all-clang

all: $(WORKER_BIN) $(CONTROL_BIN)

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(WORKER_BIN): examples/worker_app.c $(LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(CONTROL_BIN): examples/control_app.c $(LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 7.3 — Статический анализ (clang scan-build, ноль предупреждений)
analyze:
	scan-build --status-bugs $(MAKE) clean all

# 7.5 — Сборка двумя компиляторами с флагами безопасности
all-gcc:
	@echo "=== gcc + security flags ==="
	gcc $(CFLAGS) $(CFLAGS_SEC) $(LIB_SRCS) examples/worker_app.c \
	    -o worker_app_gcc  $(LDFLAGS) $(LDFLAGS_SEC)
	gcc $(CFLAGS) $(CFLAGS_SEC) $(LIB_SRCS) examples/control_app.c \
	    -o control_app_gcc $(LDFLAGS) $(LDFLAGS_SEC)
	@echo "OK: worker_app_gcc  control_app_gcc"

all-clang:
	@echo "=== clang + security flags ==="
	clang $(CFLAGS) $(CFLAGS_SEC) $(LIB_SRCS) examples/worker_app.c \
	    -o worker_app_clang  $(LDFLAGS) $(LDFLAGS_SEC)
	clang $(CFLAGS) $(CFLAGS_SEC) $(LIB_SRCS) examples/control_app.c \
	    -o control_app_clang $(LDFLAGS) $(LDFLAGS_SEC)
	@echo "OK: worker_app_clang  control_app_clang"

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

# Scalability benchmark: 1 worker with 1/2/4/8 cores, N0=500M
bench: all
	@echo "=== Scalability benchmark (1 worker, N0=500M) ==="
	@for cores in 1 2 4 8; do \
		./$(WORKER_BIN) 9301 $$cores 60 & \
		sleep 0.1; \
		printf "cores=%-2d  " $$cores; \
		./$(CONTROL_BIN) 1 60 127.0.0.1:9301 500000000 2>/dev/null | grep -E "Time|Error"; \
	done

clean:
	rm -f $(LIB_OBJS) $(LIB) $(WORKER_BIN) $(CONTROL_BIN) \
	      worker_app_gcc control_app_gcc \
	      worker_app_clang control_app_clang
	rm -rf docs
