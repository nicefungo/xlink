CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g
INCS    = -I include -I third_party/shm_ipc/include
LDLIBS  = -L third_party/shm_ipc/bin -lshm_ipc -lpthread -lrt

LIB     = bin/libxlink.a
OBJS    = bin/xlink.o bin/shm_backend.o bin/pipe_backend.o \
          bin/tcp_backend.o bin/udp_backend.o bin/serial_backend.o bin/file_backend.o

SRCDIR  = src

all: lib tests

lib: $(OBJS)
	$(AR) rcs $(LIB) $(OBJS)

$(OBJS): bin/%.o: $(SRCDIR)/%.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCS) -o $@ -c $<

tests: lib
	@mkdir -p bin/tests
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_shm       tests/test_shm.c       $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_pipe      tests/test_pipe.c      $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp       tests/test_tcp.c       $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_udp       tests/test_udp.c       $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_serial    tests/test_serial.c    $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_file      tests/test_file.c      $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_wait      tests/test_wait.c      $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_errors    tests/test_errors.c    $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_broadcast tests/test_broadcast.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_rawio     tests/test_rawio.c     $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_api_util  tests/test_api_util.c  $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_frame_overflow tests/test_frame_overflow.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_empty tests/test_tcp_empty.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_wait_edge tests/test_wait_edge.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_nonblock  tests/test_nonblock.c  $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_pipe_empty tests/test_pipe_empty.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_serial_nonblock tests/test_serial_nonblock.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_nonblock tests/test_tcp_nonblock.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_server_nonblock tests/test_tcp_server_nonblock.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_udp_edge  tests/test_udp_edge.c  $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_pipe_stress tests/test_pipe_stress.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_overflow tests/test_tcp_overflow.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_multi tests/test_tcp_multi.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_zero  tests/test_tcp_zero.c  $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_tcp_overflow_client tests/test_tcp_overflow_client.c $(LIB) $(LDLIBS)
	$(CC) $(CFLAGS) $(INCS) -o bin/tests/test_file_nonblock tests/test_file_nonblock.c $(LIB) $(LDLIBS)

test: tests
	@for t in bin/tests/*; do \
		echo "--- $$t ---"; \
		$$t; \
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
