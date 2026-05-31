CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g
INCS    = -I include -I third_party/shm_ipc/include
LDLIBS  = -L third_party/shm_ipc/bin -lshm_ipc -lpthread -lrt

LIB     = bin/libxlink.a
OBJS    = bin/xlink.o bin/plugin.o bin/aio.o bin/aio_epoll.o bin/aio_poll.o \
          bin/shm_backend.o bin/pipe_backend.o \
          bin/tcp_backend.o bin/udp_backend.o bin/serial_backend.o bin/file_backend.o

SRCDIR  = src

all: lib tests

lib: $(OBJS)
	$(AR) rcs $(LIB) $(OBJS)

$(OBJS): bin/%.o: $(SRCDIR)/%.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCS) -o $@ -c $<

TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(patsubst tests/%.c, bin/tests/%, $(TEST_SRCS))

tests: lib $(TEST_BINS) mock_plugin

bin/tests/mock_plugin.so: tests/mock_plugin.c
	@mkdir -p bin/tests
	$(CC) $(CFLAGS) $(INCS) -shared -fPIC -o $@ $<

mock_plugin: bin/tests/mock_plugin.so

bin/tests/%: tests/%.c
	@mkdir -p bin/tests
	$(CC) $(CFLAGS) $(INCS) -o $@ $< $(LIB) $(LDLIBS)

test: tests
	@for t in bin/tests/*; do \
		case "$$t" in *.so) continue ;; esac; \
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

.PHONY: all lib tests test stress clean
