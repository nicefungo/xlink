# Step 2.3 Plan: SHM Zero-Copy Completion Notification

## Goal
Integrate zero-copy completion polling with the AIO event loop so that
xlink_wait_aio() / xlink_run() can detect zero-copy send completions
without separate polling.

## Implementation

### 1. Add zc_notify_fd to channel struct
- Give each channel with zc capability a way to expose notification fd
- SHM already has FIFO fd → reuse it

### 2. Extend xlink_zc_poll() to integrate with aio
- Call zc_poll during wait_aio for channels that have pending completions
- Return zc completion count as part of wait result

### 3. Add test_zc_notify.c
- Verify that FIFO notification is triggered on send_zc
- Verify that poll sees pending completion after send_zc
- Test multi-send completion tracking

### 4. Update docs
- 04-performance.md: mark Step 2.3 as complete
- index.md: add decision log entry
