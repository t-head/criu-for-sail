CRIU Support for PPU
====================

# Requirements

PPU driver version 2.1.0 or later is required. The application must also
use PPU SDK version 2.1.0 or later at runtime.

# Checkpointing Procedure

The plugin registers hooks for each stage of the CRIU checkpoint/restore
lifecycle:

* **PAUSE_DEVICES** - Pauses the PPU devices associated with the process
  being checkpointed, while the process is still running and before CRIU
  freezes it.

* **CHECKPOINT_DEVICES** - Communicates with the application's criu-thread
  via pipe IPC to instruct the runtime to save device memory contents
  to system memory.

* **HANDLE_DEVICE_VMA** - Identifies VMAs belonging to PPU devices so that
  CRIU can properly handle these non-regular mappings during dump.

* **DUMP_EXT_FILE** - Collects kernel-side device state and serializes it
  to protobuf image files.

* **RESTORE_EXT_FILE** - Restores device nodes, deserializes the protobuf
  images, recreates device state in the kernel driver, and builds the VMA
  remapping table.

* **UPDATE_VMA_MAP** - Translates old mmap offsets to newly assigned offsets
  for all device VMAs, allowing CRIU's restorer to mmap device memory at the
  correct virtual addresses.

* **RESUME_DEVICES_LATE** - Runs after all VMAs have been restored. Finalizes
  kernel-side mappings, communicates with the application's criu-thread to
  re-establish device memory contents, and resumes PPU execution.

# Environment Variables

Variable | Description
--- | ---
`PPU_CRIU_LOG_LEVEL` | Controls plugin log verbosity (0=ERROR, 1=MSG, 2=WARN, 3=INFO, 4=DEBUG). Default is INFO.
`PPU_CRIU_RESTORE_START_INDEX` | Specifies the starting PPU device index to use on restore. Allows restoring onto a different subset of PPUs on the target system. Default is 0.
`PPU_CRIU_IPC_TIMEOUT` | Timeout in milliseconds for IPC communication with the application's criu-thread. Default is 300000 (300 seconds).

# Known Limitations

* Restore currently requires that the target system has the same or greater
  number of PPU devices as the source system.
* The PPU device topology must be compatible between the source and target systems.
