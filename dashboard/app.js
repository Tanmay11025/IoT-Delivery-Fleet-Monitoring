// Polls the gateway and updates the dashboard.
//
// Browser -> GET /vehicles (every 500 ms, feeds the live map)
//         -> GET /metrics (every second)
//         -> GET /recent, /consumers (every other second)
// Each loop waits for its previous request to finish, so a slow gateway is
// never hit by overlapping requests from the dashboard it is serving.
(function () {
  "use strict";

  const POLL_MS = 1000;
  const VEHICLE_POLL_MS = 500;
  const $ = (id) => document.getElementById(id);

  const map = new window.FleetMap($("fleet-map"), $("map-tooltip"), $("map-empty"));
  const epsChart = new window.RollingChart($("chart-eps"), "events/sec", "#4aa3ff");
  const vehiclesChart = new window.RollingChart($("chart-vehicles"), "active vehicles", "#2ecc71");

  let previousEvents = null;
  let previousTime = null;
  let failures = 0;
  let tick = 0;

  // Parse Prometheus text exposition into { "name{labels}": value }.
  function parsePrometheus(text) {
    const values = {};
    for (const line of text.split("\n")) {
      if (line === "" || line.startsWith("#")) continue;
      const space = line.lastIndexOf(" ");
      if (space === -1) continue;
      values[line.slice(0, space)] = Number(line.slice(space + 1));
    }
    return values;
  }

  async function getText(path) {
    const response = await fetch(path, { cache: "no-store" });
    if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
    return response.text();
  }

  function formatBytes(bytes) {
    const units = ["B", "KiB", "MiB", "GiB"];
    let value = bytes, unit = 0;
    while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit += 1; }
    return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
  }

  function formatDuration(seconds) {
    const s = Math.floor(seconds);
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
    if (h > 0) return `${h}h ${m}m`;
    if (m > 0) return `${m}m ${s % 60}s`;
    return `${s}s`;
  }

  function badge(status) {
    return `<span class="badge ${escapeHtml(status)}">${escapeHtml(status)}</span>`;
  }

  function setRows(tbody, rows, columns, emptyText) {
    tbody.innerHTML = rows.length
      ? rows.join("")
      : `<tr><td class="empty-row" colspan="${columns}">${emptyText}</td></tr>`;
  }

  function renderMetrics(m) {
    // Use the gateway's own uptime as the time base: counter and clock come
    // from the same scrape, so the rate is right even when the browser
    // throttles timers in a background tab. A restart resets uptime and
    // counters, which the >= checks below skip.
    const now = m["process_uptime_seconds"] || 0;
    const events = m["gateway_telemetry_events_total"] || 0;
    let eps = 0;
    if (previousEvents !== null && now > previousTime && events >= previousEvents) {
      eps = (events - previousEvents) / (now - previousTime);
    }
    previousEvents = events;
    previousTime = now;

    const active = m["gateway_active_vehicles"] || 0;
    $("kpi-active").textContent = active;
    $("kpi-breakdown").textContent =
      `${m['gateway_vehicles{status="moving"}'] || 0} moving · ` +
      `${m['gateway_vehicles{status="idle"}'] || 0} idle · ` +
      `${m['gateway_vehicles{status="offline"}'] || 0} offline`;
    $("kpi-eps").textContent = eps.toFixed(eps < 10 ? 1 : 0);
    $("kpi-events-total").textContent = `${events.toLocaleString()} total`;

    const limited = m["gateway_http_requests_blocked_total"] || 0;
    const invalid = m["gateway_telemetry_rejected_total"] || 0;
    $("kpi-rejected").textContent = (limited + invalid).toLocaleString();
    $("kpi-rejected-sub").textContent = `${limited} rate-limited · ${invalid} invalid`;

    const workers = m["gateway_thread_pool_workers"] || 0;
    const busy = m["gateway_thread_pool_busy_threads"] || 0;
    $("kpi-pool").textContent = `${busy}/${workers}`;
    $("kpi-pool-sub").textContent =
      `busy / workers · queue ${m["gateway_thread_pool_queue_length"] || 0}`;

    $("kpi-log").textContent = formatBytes(m["gateway_log_end_offset"] || 0);
    $("kpi-log-sub").textContent = `${formatBytes(m["gateway_log_disk_bytes"] || 0)} provisioned`;

    $("kpi-consumers").textContent = m["gateway_consumers_connected"] || 0;
    $("kpi-lag").textContent = `lag ${formatBytes(m["gateway_consumer_lag_bytes"] || 0)}`;

    $("uptime").textContent = formatDuration(m["process_uptime_seconds"] || 0);

    epsChart.push(Number(eps.toFixed(1)));
    vehiclesChart.push(active);
  }

  let lastTableRender = 0;

  function renderVehicles(data, receivedAt) {
    map.update(data.vehicles, receivedAt);
    // The map wants every report; the table (which may hold thousands of
    // rows) only needs refreshing once a second.
    if (receivedAt - lastTableRender < 900) return;
    lastTableRender = receivedAt;
    $("vehicle-count").textContent = `${data.counts.total} known`;
    const rows = data.vehicles.map((v) =>
      `<tr><td>${escapeHtml(v.vehicle_id)}</td><td>${badge(v.status)}</td>` +
      `<td class="num">${v.speed.toFixed(1)}</td><td class="num">${v.lat.toFixed(4)}</td>` +
      `<td class="num">${v.lng.toFixed(4)}</td><td class="num">${v.age_seconds.toFixed(0)} s</td></tr>`);
    setRows($("vehicle-rows"), rows, 6, "No vehicles reported yet");
  }

  function renderRecent(records) {
    const rows = records.slice().reverse().slice(0, 25).map((record) => {
      let event = {};
      try { event = JSON.parse(record.payload); } catch (error) { /* non-telemetry record */ }
      const received = new Date(record.timestamp_ns / 1e6).toLocaleTimeString([], { hour12: false });
      return `<tr><td class="num">${record.offset}</td>` +
        `<td>${escapeHtml(event.vehicle_id || "–")}</td>` +
        `<td>${event.status ? badge(event.status) : "–"}</td>` +
        `<td class="num">${typeof event.speed === "number" ? event.speed.toFixed(1) : "–"}</td>` +
        `<td class="num">${received}</td></tr>`;
    });
    setRows($("recent-rows"), rows, 5, "Nothing published yet");
  }

  function renderConsumers(data) {
    const rows = data.consumers.map((c) =>
      `<tr><td>#${c.id}</td><td>${escapeHtml(c.client_ip)}${c.streaming ? "" : " (idle)"}</td>` +
      `<td class="num">${c.cursor}</td><td class="num">${c.acked}</td>` +
      `<td class="num">${formatBytes(c.lag_bytes)}</td></tr>`);
    setRows($("consumer-rows"), rows, 5, "No consumers connected");
  }

  function setConnected(connected) {
    const pill = $("connection-pill");
    if (connected) {
      pill.textContent = "Live";
      pill.className = "pill pill-live";
      $("banner").hidden = true;
      $("last-update").textContent = new Date().toLocaleTimeString([], { hour12: false });
    } else {
      pill.textContent = "Reconnecting…";
      pill.className = "pill pill-down";
      $("banner").hidden = false;
      // Rates computed across an outage would be misleading.
      previousEvents = null;
    }
  }

  async function poll() {
    tick += 1;
    try {
      renderMetrics(parsePrometheus(await getText("/metrics")));
      // The tables change slowly; refresh them every other tick.
      if (tick % 2 === 1) {
        const [recentText, consumersText] = await Promise.all([
          getText("/recent?count=50"), getText("/consumers")]);
        renderRecent(JSON.parse(recentText));
        renderConsumers(JSON.parse(consumersText));
      }
      failures = 0;
      setConnected(true);
    } catch (error) {
      failures += 1;
      // A single 429 or dropped request is not an outage.
      if (failures >= 2) setConnected(false);
      console.warn("poll failed:", error.message);
    } finally {
      setTimeout(poll, POLL_MS);
    }
  }

  // The map interpolates between reports, so it wants them promptly; see
  // fleet_map.js. Connection state is tracked by the main loop.
  async function pollVehicles() {
    try {
      const text = await getText("/vehicles");
      renderVehicles(JSON.parse(text), performance.now());
    } catch (error) {
      console.warn("vehicle poll failed:", error.message);
    } finally {
      setTimeout(pollVehicles, VEHICLE_POLL_MS);
    }
  }

  poll();
  pollVehicles();
})();
