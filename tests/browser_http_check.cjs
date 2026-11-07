"use strict";
// Run with Playwright on NODE_PATH; uses installed Edge, no browser download.
const { chromium } = require("playwright");
const path = require("node:path");
const fs = require("node:fs");

(async () => {
  const output = path.resolve(process.argv[2] || "local/http-host-build");
  fs.mkdirSync(output, { recursive: true });
  const traceStarted = process.hrtime.bigint();
  const expectedBackend = process.argv?.[4] || "";
  const provenance = { source: "DOM labels only; SDK/firmware identity belongs to the external flash manifest",
    expected_backend: expectedBackend || null, limit: 8, dropped: 0, pages: [] };
  const classifyTransport = label => {
    const tcp = /\bshared TCP\b/i.test(label);
    const httpd = /\bESP-IDF HTTPD\b/i.test(label);
    return tcp === httpd ? "unknown" : tcp ? "tcp" : "httpd";
  };
  const recordProvenance = (phase, result) => {
    const transportLabel = String(result.transportLabel || "");
    const backendLabel = String(result.backendLabel || "");
    const actualBackend = classifyTransport(transportLabel);
    const bounded = transportLabel.length <= 256 && backendLabel.length <= 256;
    const matches = expectedBackend ? bounded && actualBackend === expectedBackend : null;
    if (provenance.pages.length < provenance.limit) provenance.pages.push({ phase,
      elapsed_ms: Number(process.hrtime.bigint() - traceStarted) / 1e6,
      transport_label: transportLabel.slice(0, 256), backend_label: backendLabel.slice(0, 256),
      actual_backend: actualBackend, labels_bounded: bounded, matches_expected: matches });
    else provenance.dropped++;
    if (expectedBackend && !matches) throw new Error(`Backend provenance mismatch in ${phase}: ` +
      `expected ${expectedBackend}, transport=${JSON.stringify(transportLabel.slice(0, 256))}, ` +
      `backend=${JSON.stringify(backendLabel.slice(0, 256))}, classified=${actualBackend}`);
  };
  const websocketTrace = { limit: 256, dropped: 0, socketsCreated: 0, provenance, records: [] };
  const networkTrace = { limit: 512, retention: "last", trimmed: 0, dropped: 0, totalRecords: 0,
    requestsCreated: 0, untrackedRequests: 0, extraInfoUntracked: 0, setupFailures: 0, provenance, records: [] };
  let networkNext = 0;
  const eventSourceTrace = { limit: 256, dropped: 0, untrackedMessages: 0, setupFailures: 0, provenance, records: [] };
  const knownPaths = new Set(["/", "/http-test.css", "/http-test.js", "/mcp-client.js", "/mcp-panel.js", "/mcp", "/http-test-config.json", "/favicon.ico",
    "/health", "/echo", "/private/body", "/private", "/not-found", "/ws", "/events", "/events/private",
    "/events/publish", "/events/close", "/events/stats"]);
  const eventSourcePaths = new Set(["/events", "/events/private"]);
  const knownPath = url => {
    try {
      const pathname = new URL(url).pathname;
      return knownPaths.has(pathname) ? pathname : null;
    } catch (_) { return null; }
  };
  const recordNetwork = (metadata, event, details = {}) => {
    const entry = { sequence: ++networkTrace.totalRecords, ...metadata, event,
      elapsed_ms: Number(process.hrtime.bigint() - traceStarted) / 1e6, ...details };
    if (networkTrace.records.length < networkTrace.limit) {
      networkTrace.records.push(entry);
    } else {
      networkTrace.records[networkNext] = entry;
      networkNext = (networkNext + 1) % networkTrace.limit;
      networkTrace.trimmed++;
    }
  };
  const networkTraceSnapshot = () => ({ ...networkTrace, records: networkTrace.trimmed ?
    networkTrace.records.slice(networkNext).concat(networkTrace.records.slice(0, networkNext)) :
    networkTrace.records.slice() });
  const traceNetwork = async (page, currentPhase) => {
    // Public Playwright CDPSession API, verified against the installed 1.56.1
    // types. Trace only metadata; enabling this domain sends no HTTP requests.
    const requests = new Map();
    const pendingExtraInfo = new Map();
    try {
      const session = await page.context().newCDPSession(page);
      session.on("Network.requestWillBeSent", event => {
        const pathname = knownPath(event.request.url);
        if (!pathname) {
          pendingExtraInfo.delete(event.requestId);
          return;
        }
        let metadata = requests.get(event.requestId);
        if (!metadata) {
          if (requests.size >= networkTrace.limit) {
            networkTrace.untrackedRequests++;
            return;
          }
          metadata = { request: ++networkTrace.requestsCreated, phase: currentPhase(), path: pathname,
            method: String(event.request.method).slice(0, 16) };
          requests.set(event.requestId, metadata);
        }
        recordNetwork(metadata, "request");
        const extra = pendingExtraInfo.get(event.requestId);
        if (extra) {
          recordNetwork(metadata, "requestExtraInfo", extra);
          pendingExtraInfo.delete(event.requestId);
        }
      });
      session.on("Network.requestWillBeSentExtraInfo", event => {
        // ExtraInfo can arrive before the main event. Preserve only these
        // numeric fields, never headers/cookies/security state or IP addresses.
        // Presence/requestTime is not proof that bytes reached the wire or MCU.
        const extra = { extra_received_ms: Number(process.hrtime.bigint() - traceStarted) / 1e6 };
        if (Number.isFinite(event.connectTiming?.requestTime))
          extra.connect_request_time = event.connectTiming.requestTime;
        const metadata = requests.get(event.requestId);
        if (metadata) recordNetwork(metadata, "requestExtraInfo", extra);
        else if (pendingExtraInfo.size < networkTrace.limit || pendingExtraInfo.has(event.requestId))
          pendingExtraInfo.set(event.requestId, extra);
        else networkTrace.extraInfoUntracked++;
      });
      session.on("Network.responseReceived", event => {
        const metadata = requests.get(event.requestId);
        if (metadata) {
          const details = { status: event.response.status };
          if (typeof event.response.connectionReused === "boolean")
            details.connection_reused = event.response.connectionReused;
          if (Number.isFinite(event.response.connectionId)) details.connection_id = event.response.connectionId;
          if (event.response.timing) {
            // CDP requestTime is seconds; the other fields are relative ms.
            // -1 means unavailable. These are Chromium send-phase timings for
            // a request with a response, not packet capture/server receipt.
            details.timing = {};
            for (const field of ["requestTime", "sendStart", "sendEnd", "receiveHeadersStart"])
              if (Number.isFinite(event.response.timing[field])) details.timing[field] = event.response.timing[field];
          }
          recordNetwork(metadata, "response", details);
        }
      });
      session.on("Network.loadingFailed", event => {
        const metadata = requests.get(event.requestId);
        if (metadata) {
          const code = String(event.errorText).match(/net::ERR_[A-Z0-9_]+/);
          recordNetwork(metadata, "failure", { error: code ? code[0] : "network-error", canceled: !!event.canceled });
        }
        requests.delete(event.requestId);
        pendingExtraInfo.delete(event.requestId);
      });
      session.on("Network.loadingFinished", event => {
        const metadata = requests.get(event.requestId);
        if (metadata) recordNetwork(metadata, "finished");
        requests.delete(event.requestId);
        pendingExtraInfo.delete(event.requestId);
      });
      session.on("Network.eventSourceMessageReceived", event => {
        const metadata = requests.get(event.requestId);
        if (!metadata || !eventSourcePaths.has(metadata.path)) {
          eventSourceTrace.untrackedMessages++;
          return;
        }
        if (eventSourceTrace.records.length >= eventSourceTrace.limit) {
          eventSourceTrace.dropped++;
          return;
        }
        eventSourceTrace.records.push({ sequence: eventSourceTrace.records.length + 1, ...metadata,
          elapsed_ms: Number(process.hrtime.bigint() - traceStarted) / 1e6,
          event_name: String(event.eventName).slice(0, 64), data_bytes: Buffer.byteLength(event.data, "utf8"),
          data_has_nul: event.data.includes("\0") });
      });
      await session.send("Network.enable");
    } catch (_) {
      // A diagnostic setup failure must not replace the existing test result.
      networkTrace.setupFailures++;
      eventSourceTrace.setupFailures++;
    }
  };
  const recordWebSocket = (socket, phase, event, payload) => {
    if (websocketTrace.records.length >= websocketTrace.limit) {
      websocketTrace.dropped++;
      return;
    }
    const entry = { sequence: websocketTrace.records.length + 1, socket, phase, event,
      elapsed_ms: Number(process.hrtime.bigint() - traceStarted) / 1e6 };
    if (event === "frameSent" || event === "frameReceived") {
      entry.payload_kind = typeof payload === "string" ? "text" : Buffer.isBuffer(payload) ? "binary" : "unknown";
      entry.payload_bytes = typeof payload === "string" ? Buffer.byteLength(payload, "utf8") :
        Buffer.isBuffer(payload) ? payload.length : null;
      entry.known_tick = typeof payload === "string" && payload === "tick";
    } else if (event === "socketerror") {
      entry.error = String(payload).slice(0, 240);
    }
    websocketTrace.records.push(entry);
  };
  const traceWebSockets = (page, currentPhase) => {
    page.on("websocket", websocket => {
      const socket = ++websocketTrace.socketsCreated;
      const phase = currentPhase(); // Fixed for the entire lifetime of this socket.
      recordWebSocket(socket, phase, "created");
      websocket.on("framesent", event => recordWebSocket(socket, phase, "frameSent", event.payload));
      websocket.on("framereceived", event => recordWebSocket(socket, phase, "frameReceived", event.payload));
      websocket.on("socketerror", error => recordWebSocket(socket, phase, "socketerror", error));
      websocket.on("close", () => recordWebSocket(socket, phase, "close"));
    });
  };
  let browser;
  try {
    if (expectedBackend && !["tcp", "httpd"].includes(expectedBackend))
      throw new Error("Optional expected backend argv[4] must be tcp or httpd");
    browser = await chromium.launch({ channel: "msedge", headless: true });
    const page = await browser.newPage({ viewport: { width: 1180, height: 760 }, colorScheme: "light" });
    let normalPhase = "initial";
    traceWebSockets(page, () => normalPhase);
    await traceNetwork(page, () => normalPhase);
    const errors = [];
    const browserMessages = [];
    const publishProbe = { injected: 0, attempts: 0, responses: [] };
    const publishRequests = new Set();
    const publishResponses = [];
    let queuedObserved;
    const queuedSeen = new Promise(resolve => { queuedObserved = resolve; });
    let publishSignature = null;
    let probeActive = true;
    await page.route("**/events/publish?*", async route => {
      const request = route.request();
      const signature = request.url() + "\n" + request.postData();
      if (probeActive && publishSignature === null) publishSignature = signature;
      if (probeActive && signature === publishSignature) {
        publishRequests.add(request);
        publishProbe.attempts++;
        if (publishProbe.injected === 0) {
          publishProbe.injected++;
          await route.fulfill({ status: 409, contentType: "text/plain", body: "busy" });
          return; // No server request or accepted event exists for this attempt.
        }
      }
      await route.continue();
    });
    page.on("response", response => {
      if (publishRequests.has(response.request())) publishResponses.push((async () => {
        const value = { status: response.status(), body: await response.text() };
        publishProbe.responses.push(value);
        if (value.status === 200 && value.body === "queued") queuedObserved();
      })());
    });
    page.on("pageerror", error => errors.push(error.message));
    page.on("console", message => { if (message.type() === "error") browserMessages.push(message.text()); });
    const response = await page.goto(process.argv[3] || "http://127.0.0.1:8080/", { waitUntil: "load" });
    if (response.status() !== 200) throw new Error(`Navigation HTTP ${response.status()}`);
    await page.waitForFunction(() => ["pass", "fail"].includes(document.body.dataset.result), null, { timeout: 30000 });
    const result = await page.evaluate(() => ({ result: document.body.dataset.result, passed: document.body.dataset.passed,
      summary: document.querySelector("#summary").textContent, total: document.querySelectorAll("#results tr").length,
      transportLabel: document.querySelector("#transport")?.textContent || "",
      backendLabel: document.querySelector("#backend")?.textContent || "",
      failures: [...document.querySelectorAll("#results tr")].filter(row => row.cells[6].textContent === "FAIL").map(row => row.cells[0].textContent),
      css: [...document.styleSheets].map(sheet => sheet.href),
      rowHeights: [...document.querySelectorAll("#results tr")].map(row => row.getBoundingClientRect().height) }));
    recordProvenance("initial", result);
    if (result.total === 18) {
      let timer;
      try {
        await Promise.race([queuedSeen, new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error("No accepted publish response")), 3000);
        })]);
      } finally { clearTimeout(timer); }
      await Promise.all(publishResponses);
      const queued = publishProbe.responses.filter(response => response.status === 200 && response.body === "queued");
      const busy = publishProbe.responses.filter(response => response.status === 409 && response.body === "busy");
      if (publishProbe.injected !== 1 || queued.length !== 1 || busy.length < 1 ||
          publishProbe.attempts !== queued.length + busy.length)
        throw new Error("Explicit Busy retry or accepted-payload replay check failed: " + JSON.stringify(publishProbe));
    }
    probeActive = false;
    await page.screenshot({ path: path.join(output, "http-browser-desktop.png"), fullPage: true });
    console.log(JSON.stringify({ ...result, errors, browserMessages, publishProbe }, null, 2));
    normalPhase = "repeat";
    await page.locator("#run").click();
    await page.waitForFunction(() => document.body.dataset.result !== "running");
    const repeatResult = await page.evaluate(() => ({ result: document.body.dataset.result,
      passed: document.body.dataset.passed, total: document.querySelectorAll("#results tr").length,
      transportLabel: document.querySelector("#transport")?.textContent || "",
      backendLabel: document.querySelector("#backend")?.textContent || "",
      failures: [...document.querySelectorAll("#results tr")].filter(row => row.cells[6].textContent === "FAIL")
        .map(row => ({ name: row.cells[0].textContent, detail: row.textContent })),
      detail: document.querySelector("#detail").textContent }));
    recordProvenance("repeat", repeatResult);
    console.log(JSON.stringify({ repeatResult }, null, 2));
    if (repeatResult.result !== "pass" || repeatResult.passed !== String(repeatResult.total) ||
        repeatResult.total !== result.total) {
      await page.screenshot({ path: path.join(output, "http-browser-repeat-failure.png"), fullPage: true });
      throw new Error("Repeat run failed: " + JSON.stringify(repeatResult));
    }
    await page.setViewportSize({ width: 430, height: 780 });
    await page.screenshot({ path: path.join(output, "http-browser-mobile.png"), fullPage: true });
    const layout = await page.evaluate(() => ({ pageOverflow: document.documentElement.scrollWidth > innerWidth,
      tableScrolls: document.querySelector(".table-scroll").scrollWidth > document.querySelector(".table-scroll").clientWidth }));
    await page.emulateMedia({ colorScheme: "dark" });
    await page.setViewportSize({ width: 1180, height: 760 });
    await page.screenshot({ path: path.join(output, "http-browser-dark.png"), fullPage: true });
    console.log(JSON.stringify({ ...result, errors, layout }, null, 2));
    if (result.result !== "pass" || ![12, 14, 18].includes(result.total) || result.passed !== String(result.total) || errors.length || layout.pageOverflow || !layout.tableScrolls ||
      !result.css.some(url => url.endsWith("/http-test.css"))) throw new Error("Browser HTTP or layout checks failed");
    if (result.total === 18) {
      // Refusal fixtures run sequentially on the MCU too: release the normal
      // page's connection pool before opening an independent browser context.
      await page.close();
      for (const [refusalIndex, refusal] of [{ status: 409, body: "closed" }, { status: 503, body: "busy" },
        { status: 409, body: "busy " }, { status: 409, body: "busy", limit: 8 }, { abort: true }].entries()) {
        const rejectedPage = await browser.newPage();
        traceWebSockets(rejectedPage, () => `refusal-${refusalIndex + 1}`);
        await traceNetwork(rejectedPage, () => `refusal-${refusalIndex + 1}`);
        let attempts = 0;
        let signature = null;
        await rejectedPage.route("**/events/publish?*", async route => {
          const request = route.request();
          const current = request.url() + "\n" + request.postData();
          if (signature === null) signature = current;
          if (current === signature) {
            attempts++;
            if (refusal.abort) await route.abort("failed");
            else await route.fulfill({ status: refusal.status, contentType: "text/plain", body: refusal.body });
          } else await route.continue();
        });
        await rejectedPage.goto(process.argv[3] || "http://127.0.0.1:8080/", { waitUntil: "load" });
        await rejectedPage.waitForFunction(() => ["pass", "fail"].includes(document.body.dataset.result), null,
          { timeout: 30000 });
        const rejected = await rejectedPage.evaluate(() => ({ result: document.body.dataset.result,
          transportLabel: document.querySelector("#transport")?.textContent || "",
          backendLabel: document.querySelector("#backend")?.textContent || "",
          failures: [...document.querySelectorAll("#results tr")].filter(row => row.cells[6].textContent === "FAIL")
            .map(row => row.cells[0].textContent), detail: document.querySelector("#detail").textContent }));
        recordProvenance(`refusal-${refusalIndex + 1}`, rejected);
        await rejectedPage.close();
        const expectedError = refusal.abort ? rejected.detail.toLowerCase().includes("fetch") :
          rejected.detail.includes(`Publish HTTP ${refusal.status}: ${refusal.body}`);
        if (rejected.result !== "fail" || rejected.failures.length !== 1 || attempts !== (refusal.limit || 1) ||
            !expectedError)
          throw new Error("Unexpected refusal retry/masking: " + JSON.stringify({ refusal, attempts, rejected }));
        console.log(JSON.stringify({ refusalProbe: refusal, attempts, rejected }, null, 2));
      }
    }
  } finally {
    try { if (browser) await browser.close(); }
    finally {
      fs.writeFileSync(path.join(output, "http-browser-websocket-trace.json"),
        JSON.stringify(websocketTrace, null, 2) + "\n");
      fs.writeFileSync(path.join(output, "http-browser-network-trace.json"),
        JSON.stringify(networkTraceSnapshot(), null, 2) + "\n");
      fs.writeFileSync(path.join(output, "http-browser-sse-trace.json"),
        JSON.stringify(eventSourceTrace, null, 2) + "\n");
    }
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
