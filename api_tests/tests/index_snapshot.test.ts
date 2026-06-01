import { afterAll, describe, expect, it, setDefaultTimeout } from "bun:test";
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { Phases } from "../src/constants";
import { TypesenseProcessManager } from "../src/manager";
import { fetchSingleNode } from "../src/request";

setDefaultTimeout(120_000);

const BASE_DIR = join(process.cwd(), "./data/index-snapshot-api");
const SNAPSHOT_DIR = join(BASE_DIR, "idxsnap");
const OPERATION_SNAPSHOT_DIR = join(BASE_DIR, "snapshot", "single-node");
const DATA_DIR = "typesense-data";
const PORT = 18108;
const PEERING_PORT = 18107;
const NODE_NAME = "index-snapshot-node";
const COLLECTION_NAME = "api_index_snapshot_products";

let manager: TypesenseProcessManager | null = null;

function logPath() {
  return join(BASE_DIR, "logs", "typesense", "typesense.log");
}

async function waitForCondition(
  label: string,
  predicate: () => boolean,
  timeoutMs = 20_000,
  intervalMs = 250,
) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) {
      return;
    }
    await Bun.sleep(intervalMs);
  }
  throw new Error(`Timed out waiting for ${label}`);
}

function hasSnapshotFile() {
  if (!existsSync(SNAPSHOT_DIR)) {
    return false;
  }
  return readdirSync(SNAPSHOT_DIR).some((entry) => entry.endsWith(".idxsnap"));
}

function logContains(text: string) {
  try {
    return readFileSync(logPath(), "utf8").includes(text);
  } catch {
    return false;
  }
}

async function startSnapshotNode() {
  manager = new TypesenseProcessManager(BASE_DIR, process.env.TYPESENSE_BINARY_PATH!, {
    additionalConfigs: [
      "--enable-index-snapshot",
      `--index-snapshot-dir=${SNAPSHOT_DIR}`,
    ],
  });
  await manager.startSingleNode(DATA_DIR, PORT, PEERING_PORT, NODE_NAME);
}

async function stopSnapshotNode() {
  if (!manager) {
    return;
  }
  await manager.stopServer(NODE_NAME);
  manager = null;
}

async function searchProducts() {
  const query = new URLSearchParams({
    q: "jeep",
    query_by: "title",
    filter_by: "category:=vehicles && price:<25",
    facet_by: "category",
    vector_query: "embedding:([0.1,0.2,0.3], k:1)",
  });
  const res = await fetchSingleNode(`/collections/${COLLECTION_NAME}/documents/search?${query}`, {
    method: "GET",
  }, PORT);
  expect(res.ok).toBe(true);
  return res.json();
}

async function getProduct(id: string) {
  const res = await fetchSingleNode(`/collections/${COLLECTION_NAME}/documents/${id}`, {
    method: "GET",
  }, PORT);
  expect(res.ok).toBe(true);
  return res.json();
}

afterAll(async () => {
  await stopSnapshotNode();
});

describe(Phases.NO_PHASE, () => {
  it("restores a collection from an index snapshot on server restart", async () => {
    rmSync(BASE_DIR, { recursive: true, force: true });
    mkdirSync(SNAPSHOT_DIR, { recursive: true });

    await startSnapshotNode();

    let res = await fetchSingleNode("/collections", {
      method: "POST",
      body: JSON.stringify({
        name: COLLECTION_NAME,
        fields: [
          { name: "title", type: "string" },
          { name: "category", type: "string", facet: true },
          { name: "price", type: "int32", facet: true },
          { name: "embedding", type: "float[]", num_dim: 3 },
        ],
        default_sorting_field: "price",
      }),
    }, PORT);
    expect(res.ok).toBe(true);

    for (const document of [
      { id: "1", title: "fast red jeep", category: "vehicles", price: 19, embedding: [0.1, 0.2, 0.3] },
      { id: "2", title: "slow blue truck", category: "vehicles", price: 29, embedding: [0.8, 0.1, 0.1] },
      { id: "3", title: "quiet green train", category: "rail", price: 9, embedding: [0.0, 0.9, 0.1] },
    ]) {
      res = await fetchSingleNode(`/collections/${COLLECTION_NAME}/documents`, {
        method: "POST",
        body: JSON.stringify(document),
      }, PORT);
      expect(res.ok).toBe(true);
    }

    const beforeRestart = await searchProducts();
    expect(beforeRestart.found).toBe(1);
    expect(beforeRestart.hits[0].document.id).toBe("1");
    expect(beforeRestart.facet_counts[0].counts[0].value).toBe("vehicles");

    res = await fetchSingleNode(`/collections/${COLLECTION_NAME}/documents/1`, {
      method: "PATCH",
      body: JSON.stringify({ title: "fast red jeep refreshed", price: 18 }),
    }, PORT);
    expect(res.ok).toBe(true);

    await manager?.createSnapshot(PORT, OPERATION_SNAPSHOT_DIR);
    await manager?.stopServer(NODE_NAME, "SIGTERM");
    manager = null;

    await waitForCondition("index snapshot file", hasSnapshotFile);
    expect(logContains("Saving index snapshots before worker shutdown")).toBe(true);
    expect(logContains(`Saved index snapshot for collection ${COLLECTION_NAME}`)).toBe(true);

    const snapshotFile = readdirSync(SNAPSHOT_DIR).find((entry) =>
      /^\d+\.idxsnap$/.test(entry)
    );
    expect(snapshotFile).toBeDefined();
    const snapshotPath = join(SNAPSHOT_DIR, snapshotFile!);
    const staleSnapshotPath = join(SNAPSHOT_DIR, "999999.idxsnap");
    const staleTmpSnapshotPath = join(SNAPSHOT_DIR, "999999.idxsnap.tmp");
    const interruptedSavePayloadPath = join(SNAPSHOT_DIR, "999999.idxsnap.tmp.payload.ABCDEF");
    const interruptedLoadPayloadPath = `${snapshotPath}.payload.ABCDEF`;
    const unrelatedSnapshotPath = join(SNAPSHOT_DIR, "not-a-collection.idxsnap");
    writeFileSync(staleSnapshotPath, "stale");
    writeFileSync(staleTmpSnapshotPath, "stale tmp");
    writeFileSync(interruptedSavePayloadPath, "stale save payload");
    writeFileSync(interruptedLoadPayloadPath, "stale load payload");
    writeFileSync(unrelatedSnapshotPath, "leave me alone");
    expect(existsSync(staleSnapshotPath)).toBe(true);
    expect(existsSync(staleTmpSnapshotPath)).toBe(true);
    expect(existsSync(interruptedSavePayloadPath)).toBe(true);
    expect(existsSync(interruptedLoadPayloadPath)).toBe(true);

    await startSnapshotNode();
    await waitForCondition("index snapshot restore log", () =>
      logContains(`Loaded collection ${COLLECTION_NAME} from index snapshot`)
    );
    await waitForCondition("stale index snapshot cleanup", () =>
      !existsSync(staleSnapshotPath) &&
      !existsSync(staleTmpSnapshotPath) &&
      !existsSync(interruptedSavePayloadPath) &&
      !existsSync(interruptedLoadPayloadPath)
    );
    expect(existsSync(unrelatedSnapshotPath)).toBe(true);

    const afterRestart = await searchProducts();
    expect(afterRestart.found).toBe(beforeRestart.found);
    expect(afterRestart.hits[0].document.id).toBe(beforeRestart.hits[0].document.id);
    expect(afterRestart.facet_counts[0].counts[0].value).toBe("vehicles");

    const restoredDocument = await getProduct("1");
    expect(restoredDocument.title).toBe("fast red jeep refreshed");
    expect(restoredDocument.price).toBe(18);

    const snapshotMtimeBeforeSleep = statSync(snapshotPath).mtimeMs;
    await Bun.sleep(1_100);
    await stopSnapshotNode();
    expect(statSync(snapshotPath).mtimeMs).toBe(snapshotMtimeBeforeSleep);
    expect(logContains(`Index snapshot for collection ${COLLECTION_NAME} is already current; skipping rewrite.`))
      .toBe(true);

    await startSnapshotNode();
    await waitForCondition("index snapshot restore after another sleep cycle", () =>
      logContains(`Loaded collection ${COLLECTION_NAME} from index snapshot`)
    );
    const afterSecondRestart = await searchProducts();
    expect(afterSecondRestart.found).toBe(1);
    expect(afterSecondRestart.hits[0].document.id).toBe("1");
  });
});
