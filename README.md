# Data Storage Worker for ESP-IDF

Async LittleFS storage worker component for **ESP-IDF (ESP32)**

This component centralizes filesystem access behind a single worker (task/queue) so multiple parts of your firmware can request file operations without fighting over LittleFS/VFS calls.

Why? Flash filesystems are easy to misuse in concurrent firmware:
- Multiple tasks writing/reading at the same time
- Long blocking I/O in the wrong context
- Inconsistent error handling / retries

## Features

- **Async worker model**: Queue file operations to one worker.
- **LittleFS-backed**: Designed for ESP-IDF + LittleFS.
- **Centralized mount lifecycle**: Mount once, use everywhere.
- **Safer concurrency**: One lane for all FS calls.

## Notes
- Ensure your partition table includes the LittleFS partition matching `partition_label`.
- Choose `queue_size`, `task_stack_size`, and `task_priority` based on your workload.
- Paths are relative to `mount_cfg.base_path`. For example, `/sensor.txt` resolves under that base path.