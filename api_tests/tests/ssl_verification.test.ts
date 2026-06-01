import { afterAll, beforeAll, describe, expect, it } from "bun:test";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer as createHttpServer, type Server as HttpServer } from "node:http";
import { createServer as createHttpsServer, type Server as HttpsServer } from "node:https";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Phases } from "../src/constants";
import { fetchSingleNode } from "../src/request";

let tempDir: string;
let mockServer: HttpsServer;
let mockPort: number;
let ipv6MockServer: HttpServer | undefined;
let ipv6MockPort: number | undefined;

function createSelfSignedCert() {
  tempDir = mkdtempSync(join(tmpdir(), "typesense-api-ssl-"));
  const configPath = join(tempDir, "openssl.cnf");
  const keyPath = join(tempDir, "key.pem");
  const certPath = join(tempDir, "cert.pem");

  writeFileSync(configPath, [
    "[req]",
    "default_bits = 2048",
    "prompt = no",
    "default_md = sha256",
    "distinguished_name = dn",
    "x509_extensions = v3_req",
    "",
    "[dn]",
    "CN = 127.0.0.1",
    "",
    "[v3_req]",
    "subjectAltName = @alt_names",
    "",
    "[alt_names]",
    "IP.1 = 127.0.0.1",
    "DNS.1 = localhost",
    "",
  ].join("\n"));

  execFileSync("openssl", [
    "req",
    "-x509",
    "-newkey", "rsa:2048",
    "-keyout", keyPath,
    "-out", certPath,
    "-days", "2",
    "-nodes",
    "-config", configPath,
  ], { stdio: "ignore" });

  return {
    key: readFileSync(keyPath),
    cert: readFileSync(certPath),
  };
}

function proxyToMock(sslVerify: boolean) {
  return fetchSingleNode("/proxy", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      url: `https://127.0.0.1:${mockPort}/ssl-check`,
      method: "GET",
      headers: {},
      ssl_verify: sslVerify,
    }),
  });
}

function proxySseToMock(sslVerify: boolean) {
  return fetchSingleNode("/proxy_sse", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      url: `https://127.0.0.1:${mockPort}/ssl-check`,
      method: "POST",
      body: "{}",
      headers: { "content-type": "application/json" },
      ssl_verify: sslVerify,
    }),
  });
}

function proxyToIpv6Mock() {
  return fetchSingleNode("/proxy", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      url: `http://[::1]:${ipv6MockPort}/ipv6-check`,
      method: "GET",
      headers: {},
    }),
  });
}

describe(Phases.SINGLE_FRESH, () => {
  beforeAll(async () => {
    const credentials = createSelfSignedCert();
    mockServer = createHttpsServer(credentials, (_req, res) => {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
    });

    await new Promise<void>((resolve) => mockServer.listen(0, "127.0.0.1", resolve));
    mockPort = (mockServer.address() as AddressInfo).port;

    const server = createHttpServer((_req, res) => {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, family: "ipv6" }));
    });

    try {
      await new Promise<void>((resolve, reject) => {
        const onError = (error: Error) => {
          server.off("listening", onListening);
          reject(error);
        };
        const onListening = () => {
          server.off("error", onError);
          resolve();
        };

        server.once("error", onError);
        server.once("listening", onListening);
        server.listen(0, "::1");
      });
      ipv6MockServer = server;
      ipv6MockPort = (server.address() as AddressInfo).port;
    } catch {
      try {
        server.close();
      } catch {
        // IPv6 loopback is optional for this no-secrets coverage.
      }
    }
  });

  afterAll(async () => {
    if (mockServer) {
      await new Promise<void>((resolve, reject) => {
        mockServer.close((error) => error ? reject(error) : resolve());
      });
    }

    if (ipv6MockServer) {
      await new Promise<void>((resolve, reject) => {
        ipv6MockServer?.close((error) => error ? reject(error) : resolve());
      });
    }

    if (tempDir) {
      rmSync(tempDir, { recursive: true, force: true });
    }
  });

  it("should reject self-signed upstream certificates when ssl_verify is enabled", async () => {
    const relaxedRes = await proxyToMock(false);
    expect(relaxedRes.status).toBe(200);
    expect(await relaxedRes.json()).toEqual({ ok: true });

    const verifiedRes = await proxyToMock(true);
    expect(verifiedRes.status).toBe(500);
  });

  it("should return a deterministic SSE proxy error when SSL verification fails", async () => {
    const verifiedRes = await proxySseToMock(true);
    expect(verifiedRes.status).toBe(500);

    const body = await verifiedRes.json() as { message?: string };
    expect(body.message).toBe("Server error on remote server. Please try again later.");
  });

  it("should proxy to bracketed IPv6 upstream URLs without secrets", async () => {
    if (!ipv6MockServer || ipv6MockPort === undefined) {
      return;
    }

    const res = await proxyToIpv6Mock();
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ ok: true, family: "ipv6" });
  });
});
