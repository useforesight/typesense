# Index Snapshots

Typesense rebuilds each collection's in-memory `Index` from documents stored in RocksDB during startup. Index snapshots make that startup path faster by writing the already-built RAM index to an `.idxsnap` file and loading it back on the next start when it still matches the collection state.

The snapshot is a cache. RocksDB remains the source of truth for collection metadata, document bodies, sequence ids, and Raft/store durability.

## Configuration

Enable snapshots with:

```bash
TYPESENSE_ENABLE_INDEX_SNAPSHOT=TRUE
TYPESENSE_INDEX_SNAPSHOT_DIR=/path/to/index-snapshots
```

The equivalent CLI/config file options are:

```bash
--enable-index-snapshot
--index-snapshot-dir /path/to/index-snapshots
```

If no snapshot directory is configured, Typesense uses `<data-dir>/index-snapshots`.

Each collection writes one snapshot file:

```text
<index-snapshot-dir>/<collection-id>.idxsnap
```

## Refresh Behavior

Snapshots are refreshed in two places:

1. After a collection had to be rebuilt from RocksDB during startup.
2. During shutdown, after new writes are rejected, the Raft node has stopped,
   and all queued writes have drained.

Snapshots are not refreshed during normal runtime. Typesense increments an
internal per-collection `index_snapshot_version` whenever that collection's
persisted documents are created, updated, or deleted. If documents change after
the latest snapshot and the process exits without reaching the snapshot shutdown
flush, that version will no longer match. Typesense detects the mismatch and
falls back to a normal RocksDB replay, then writes a fresh snapshot after replay.
The version marker is updated even when snapshot loading is disabled so stale
snapshot files cannot become valid again after the feature is re-enabled.

Before writing, Typesense compares the existing snapshot manifest with the
current collection state. If they match, the existing snapshot is retained
instead of being rewritten. Containers that repeatedly wake and stop without
receiving writes therefore avoid spending their shutdown window serializing
unchanged indexes.

Collection metadata must already be present when `CollectionManager::load()` runs. In normal operation this comes from the Raft/RocksDB snapshot. If a node has no operational snapshot and must rebuild RocksDB from Raft log replay first, Typesense skips the index snapshot for that boot, replays from RocksDB, and writes a fresh index snapshot afterwards.

Before loading collections at startup, Typesense cleans the snapshot directory.
It removes numeric `<collection-id>.idxsnap` files whose collection id is no
longer present in RocksDB metadata, along with these interrupted temporary
files:

```text
<collection-id>.idxsnap.tmp
<collection-id>.idxsnap.tmp.payload.XXXXXX
<collection-id>.idxsnap.payload.XXXXXX
```

Files that do not match the index snapshot naming scheme are ignored. A
snapshot rejected during startup is removed before RocksDB replay starts so
repeated interrupted starts do not retain an unusable cache.

## Safety Model

The snapshot is only loaded when all of these are true:

1. The file exists and has the expected magic and binary snapshot format version.
2. The manifest matches the current collection state:
   - collection name
   - collection id
   - next sequence id
   - per-collection index snapshot version
   - full collection metadata/schema
3. The destination index is empty.
4. Every serialized field exists in the current schema.
5. Every section validates its shape against the current index structures.

The manifest's `num_documents` value is informational and is not trusted during
restore. After the snapshot body is loaded, the collection document count is set
from the restored `seq_ids` index state.

If any check fails, `CollectionManager::load_collection` logs that the snapshot was skipped, destroys the partially initialized collection, recreates it, and rebuilds from RocksDB. A bad snapshot does not replace or mutate RocksDB.

Snapshot writes are atomic at the file level:

1. Write `<collection-id>.idxsnap.tmp`.
2. Stream all sections to the temp file.
3. Flush and close the temp file.
4. Re-read the collection's `index_snapshot_version` and `next_seq_id`.
5. Rename it over `<collection-id>.idxsnap` only if both values are unchanged.

If any write or pre-commit validation step fails, Typesense deletes the temp
file and leaves the previous snapshot in place. If either value changes
immediately after the rename, Typesense removes the just-written snapshot
rather than leaving a known-stale cache file behind. RocksDB remains
authoritative in both cases.

If shutdown interrupts a startup RocksDB replay, Typesense does not publish or
save that partially rebuilt in-memory index.

## Error Behavior

Missing snapshot:

```text
Skipping index snapshot for collection <name>: Index snapshot not found.
```

Manifest mismatch, including a stale per-collection index snapshot version:

```text
Skipping index snapshot for collection <name>: Index snapshot manifest does not match current collection state. Mismatched fields: index_snapshot_version snapshot=<old> expected=<new>
```

Unsupported or old binary layout:

```text
Skipping index snapshot for collection <name>: Unsupported index snapshot version.
```

Corrupt body:

```text
Skipping index snapshot for collection <name>: <section-specific read/decode error>
```

Successful startup restore:

```text
Loaded collection <name> from index snapshot <path>
Loaded index snapshot <path> in <N> ms (<bytes> bytes).
```

Successful save after replay or shutdown flush:

```text
Saving index snapshots for <count> collection(s).
Saved index snapshot for collection <name> to <path>
Saved index snapshot <path> in <N> ms (<bytes> bytes).
Finished saving index snapshots for <count> collection(s) in <N> ms.
```

An unchanged collection reuses its existing snapshot:

```text
Index snapshot for collection <name> is already current; skipping rewrite.
```

Startup reports cleanup left behind by interrupted writes or removed
collections:

```text
Removed <count> stale or interrupted index snapshot file(s).
```

If any collection fails to save, the per-collection warning contains the
reason and the pass ends with an aggregate warning:

```text
Could not save index snapshot for collection <name>: <reason>
Finished saving index snapshots with <failures> failure(s) and <successes> successful save(s) in <N> ms.
```

## Version And Upgrade Behavior

The binary snapshot format version is independent of the Typesense release version. It only changes when the `.idxsnap` byte layout changes.

This branch writes snapshot format `2`. There is no backwards compatibility layer for older snapshot layouts. On upgrade, old snapshots are skipped, collections rebuild from RocksDB once, and fresh v2 snapshots are written after replay. This keeps upgrade behavior safe and simple.

## What Is Snapshotted

The v2 format covers the persistent query-critical in-memory structures owned by `Index`:

| Structure | Purpose | Snapshot strategy |
| --- | --- | --- |
| `search_index` | text/token ART indexes and posting offsets | serialize ART leaves, scores, ids, offsets |
| `numerical_index` | numeric and boolean filters | serialize `num_tree_t` value to ids map |
| `reference_index` | reference helper field filters | serialize `num_tree_t` value to ids map |
| `object_array_reference_index` | object-array reference lookups | serialize `(seq_id, object_index) -> ref_seq_id` |
| `range_index` | numeric range filters | serialize `NumericTrie` entries |
| `geo_range_index` | geopoint and geopoint-array radius filters | serialize `NumericTrie` geopoint cells |
| `geo_array_index` | exact distance filtering for geopoint arrays | serialize packed lat/lng arrays by seq id |
| `field_geopolygon_index` | geopolygon containment filters | serialize S2 polygons with native `S2Polygon::Encode`, rebuild S2 cell trie |
| `vector_index` | HNSW vector search for embedding/vector fields | serialize native hnswlib `saveIndex` payload per field |
| `facet_index_v4` | facet values, counts, hashes | serialize facet maps, ids, counts, hashes |
| `sort_index` | numeric/geopoint sort values | serialize seq id to sortable int64 value |
| `str_sort_index` | string sort values | serialize `adi_tree_t` id to key map |
| `infix_index` | infix lookup shards | serialize each trie set's values |
| `seq_ids` | wildcard search document ids | serialize id list |
| `field_missing_index` | tracked missing optional fields | serialize field id lists |

These `Index` members are not snapshotted because they are reconstructed from metadata or are process-local only:

| Structure | Reason |
| --- | --- |
| `search_schema` | rebuilt from collection metadata |
| `symbols_to_index`, `token_separators` | rebuilt from collection metadata |
| `store`, `thread_pool`, mutexes | runtime process state |
| sentinel maps | static process constants |
| transient search state | request-local only |

## `.idxsnap` Layout

All integer values are written as fixed-width native-endian host POD values. The current deployment targets are little-endian, but the format is not intended as a cross-endian interchange format. Strings are written as:

```text
u64 byte_length
byte[byte_length] utf8_payload
```

The file layout is strict and ordered:

```text
magic: "TSISNAP1"                    8 bytes
format_version: u32                  currently 2
manifest_size: u64
manifest_json: byte[manifest_size]
  includes informational num_documents; restore derives the live count from seq_ids

search_index_count: u64
  repeated search_index_count:
    field_name: string
    art_leaf_count: u64
      repeated art_leaf_count:
        token: string
        max_score: i64
        posting_count: u64
          repeated posting_count:
            seq_id: u32
            offsets: u32_vector

numerical_index_count: u64
  repeated:
    field_name: string
    num_tree_t payload

reference_index_count: u64
  repeated:
    field_name: string
    num_tree_t payload

object_array_reference_index_count: u64
  repeated:
    field_name: string
    entry_count: u64
      repeated:
        seq_id: u32
        object_index: u32
        ref_seq_id: u32

range_index_count: u64
  repeated:
    field_name: string
    NumericTrie payload

geo_range_index_count: u64
  repeated:
    field_name: string
    NumericTrie geopoint payload

geo_array_index_count: u64
  repeated:
    field_name: string
    entry_count: u64
      repeated:
        seq_id: u32
        point_count: u64
        packed_lat_lng: i64[point_count]

geopolygon_index_count: u64
  repeated:
    field_name: string
    seq_id_count: u64
      repeated:
        seq_id: u32
        polygon_count: u64
          repeated:
            s2_polygon_payload_size: u64
            s2_polygon_payload: byte[s2_polygon_payload_size]

vector_index_count: u64
  repeated:
    field_name: string
    num_dim: u64
    distance_type: u32
    max_elements: u64
    current_element_count: u64
    deleted_count: u64
    hnsw_payload_size: u64
    hnsw_payload: byte[hnsw_payload_size]

facet_index_v4 payload

sort_index_count: u64
  repeated:
    field_name: string
    entry_count: u64
      repeated:
        seq_id: u32
        sortable_value: i64

str_sort_index_count: u64
  repeated:
    field_name: string
    adi_tree_t payload

infix_index_count: u64
  repeated:
    field_name: string
    shard_count: u64
      repeated:
        value_count: u64
        value: string[value_count]

seq_ids: id_list payload

field_missing_index_count: u64
  repeated:
    field_name: string
    missing_seq_ids: id_list payload
```

The HNSW payload is produced by hnswlib's native `HierarchicalNSW<float>::saveIndex` and restored with the matching load constructor using `allow_replace_deleted=true`, preserving graph links, labels, vectors, max capacity, and deleted labels.

The geopolygon payload stores each `S2Polygon` using S2's native encoder. On load, Typesense decodes the polygon and rebuilds the cell trie used for candidate lookup.

## Embeddings

Startup restore does not recalculate embeddings. Embedding values are already stored in RocksDB document bodies. The snapshot stores the HNSW graph built from those values, so vector collections do not have to replay every document just to rebuild the graph.

If the snapshot is skipped, Typesense falls back to the existing replay path. That path reads the stored embedding arrays from RocksDB and rebuilds HNSW without calling remote/local embedding generation.
