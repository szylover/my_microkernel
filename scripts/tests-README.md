# Tests

This project uses a minimal smoke-test approach suitable for early kernels:
we boot the ISO in QEMU and assert expected serial output markers.

## Run

```bash
make test
```

Or run the script directly:

```bash
tests/test.sh
```

## Notes

- The kernel runs an infinite `hlt` loop, so QEMU is expected to be stopped via timeout.
- Logs are written under `build/test-logs/`.
- You can tune runtime using `QEMU_TIMEOUT_SEC=10 make test`.

### Optional command tests

- PMM command test (feeds shell input via serial stdio):
	- `TEST_PMM=1 make test`
	- Optional timeout: `QEMU_TIMEOUT_PMM_SEC=10 TEST_PMM=1 make test`

- Multiboot2 mmap command test:
	- `TEST_MMAP=1 make test`
