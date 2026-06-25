CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g
INCS    = -I include -I third_party/shm_ipc/include
LDLIBS  = -L third_party/shm_ipc/bin -lshm_ipc -lpthread -lrt

LIB     = bin/libxlink.a
OBJS    = bin/xlink.o bin/plugin.o bin/aio.o bin/aio_epoll.o bin/aio_poll.o \
          bin/aio_uring.o \
          bin/shm_backend.o bin/pipe_backend.o \
          bin/tcp_backend.o bin/udp_backend.o bin/serial_backend.o bin/file_backend.o

SRCDIR  = src

# ─── TLS (optional, requires libssl-dev) ────────────────
# Build with: make tls
# Adds -DXLINK_HAS_TLS, links -lssl -lcrypto

TLS_CFLAGS = -DXLINK_HAS_TLS
TLS_LDLIBS = -lssl -lcrypto
TLS_LIB     = bin/libxlink_tls.a
TLS_OBJS    = $(patsubst bin/%.o, bin/%.tls.o, $(OBJS)) bin/tls.tls.o

all: lib tests

lib: $(OBJS)
	$(AR) rcs $(LIB) $(OBJS)

$(OBJS): bin/%.o: $(SRCDIR)/%.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCS) -o $@ -c $<

tls: $(TLS_LIB) tls_tests
	@echo "=== TLS build complete ==="

bin/%.tls.o: $(SRCDIR)/%.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(TLS_CFLAGS) $(INCS) -o $@ -c $<

$(TLS_LIB): $(TLS_OBJS)
	$(AR) rcs $(TLS_LIB) $(TLS_OBJS)

TEST_SRCS = $(filter-out tests/test_tls.c, $(wildcard tests/test_*.c))
TEST_BINS = $(patsubst tests/%.c, bin/tests/%, $(TEST_SRCS))

tests: lib $(TEST_BINS) mock_plugin

bin/tests/mock_plugin.so: tests/mock_plugin.c
	@mkdir -p bin/tests
	$(CC) $(CFLAGS) $(INCS) -shared -fPIC -o $@ $<

mock_plugin: bin/tests/mock_plugin.so

bin/tests/%: tests/%.c
	@mkdir -p bin/tests
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LIB) $(LDLIBS)

# ─── TLS tests ────────────────────────────
tls_tests: $(TLS_LIB)
	@mkdir -p bin/tests
	$(CC) $(CFLAGS) $(TLS_CFLAGS) $(INCS) -o bin/tests/test_tls tests/test_tls.c $(TLS_LIB) $(LDLIBS) $(TLS_LDLIBS)
	@echo "=== TLS test built ==="

test: tests
	@for t in bin/tests/*; do \
		case "$$t" in *.so|*.tls.o) continue ;; esac; \
		echo "--- $$t ---"; \
		$$t || true; \
	done

stress: lib
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/stress_shm     tests/stress_shm.c     $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/stress_tcp     tests/stress_tcp.c     $(LIB) $(LDLIBS)
	@for t in bin/tests/stress_*; do \
		echo "--- $$t ---"; \
		$$t; \
	done

clean:
	rm -rf bin

.PHONY: all lib tests test tls tls_tests stress clean