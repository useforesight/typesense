# Container page size support

The GHCR Dockerfile enables the fork's existing large-page jemalloc configuration
on Linux ARM64. One ARM64 image supports kernels with 4, 16 and 64 KiB pages.
The AMD64 build keeps its existing allocator configuration.

`--with-lg-page=16` means a 64 KiB allocator page, because the value is a base-2
exponent. jemalloc's allocator page must be at least as large as the kernel page.
This build option does not change the kernel or the container host.
See the [jemalloc 5.3.0 build documentation](https://github.com/jemalloc/jemalloc/blob/5.3.0/INSTALL.md).

Build the ARM64 image on an ARM64 host:

```bash
docker build --file docker/ghcr.Dockerfile \
  --build-arg BAZELISK_SHA256=e20e8b0f4f240091b7a55bf17b9398bd4f40ee70ae0208dff95dd4c445fb4010 \
  --tag foresight-typesense:arm64-all-pages .
bash docker/page-size-smoke.sh foresight-typesense:arm64-all-pages
```

The smoke check creates an isolated container and volume. It checks built-in
embeddings, search, SIGINT snapshot creation, and search after snapshot restore.
It removes its own container and volume when it finishes. It needs Docker, curl,
and jq, plus network access for the embedding model's first download.

The ARM64 page size workflow runs this check on a 4 KiB Linux kernel for pull
requests. Run the same check on a 16 KiB host before updating downstream image
pins. A 4 KiB Linux VM on macOS can use the same ARM64 image.

## Concurrent collection startup

Parallel collection loaders use separate Sparsepp maps. Those maps share an
allocation-size table for each template specialization. The bundled header
previously wrote that table without synchronization. Another thread could read
an unfinished table and allocate too little storage.

The table now uses C++11 thread-safe local static initialization. Its sizes and
the snapshot format are unchanged. The fix applies to both CPU architectures.

Run the focused regression check with a compiler that supports ThreadSanitizer:

```bash
bash test/sanitizers/sparsepp-startup.sh
```

The check starts independent maps concurrently in 100 fresh processes. It fails
on a sanitizer race report or an incorrect field lookup. The original header
produced a race report with this check on the 16 KiB ARM64 host. The fixed header
passed all 100 processes. The separate sanitizer workflow runs it on ARM64 CI.
