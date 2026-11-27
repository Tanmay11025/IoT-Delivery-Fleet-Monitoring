// Canvas fleet map: streets of the demo city with every van on them.
//
// No map tiles or API key. The city (roads, river, parks, depots) comes from
// /dashboard/city_map.json, the same file the simulator routes vans on, and
// is drawn into an off-screen layer that is only redrawn when the view
// changes. Vans are drawn on top at the display's frame rate. Scroll to zoom
// (street names appear as they fit), drag to pan, double-click to reset.
//
// Smooth, honest motion: each report is placed on a timeline at the moment
// the gateway received it (poll time minus the gateway's age_seconds). The
// map is drawn a fixed RENDER_DELAY behind real time and each van is
// interpolated between the two reports that bracket that instant. Vans
// therefore glide between positions they actually reported, never guessing
// ahead, and are always within a couple of seconds of the latest data.
// Gaps longer than SNAP_GAP_MS (lost signal) jump instead of gliding through
// buildings.
(function () {
  "use strict";

  const COLORS = { moving: "#2ecc71", idle: "#f1c40f", offline: "#e5534b" };
  const RENDER_DELAY_MS = 1800;   // one report interval + one poll + margin
  const SNAP_GAP_MS = 4000;
  const TRAIL_MS = 10000;
  const HISTORY_MS = TRAIL_MS + RENDER_DELAY_MS + 4000;
  const MIN_SPAN_DEGREES = 0.01;
  const METRES_PER_DEGREE = 111320;
  const MAX_ZOOM = 16;
  const ZOOM_STEP = 1.25;         // 25% per mouse-wheel notch
  const MAP_STYLE = {
    land: "#121820",
    water: "#0f2a3d",
    waterLabel: "#4f7896",
    park: "#122a1c",
    parkLabel: "#4c8a5f",
    casing: "#06090c",
    major: "#4a5869",
    minor: "#2c3643",
    label: "#7d8a99",
    district: "rgba(160, 175, 195, 0.32)",
  };

  class FleetMap {
    constructor(canvas, tooltip, emptyMessage) {
      this.canvas = canvas;
      this.context = canvas.getContext("2d");
      this.tooltip = tooltip;
      this.emptyMessage = emptyMessage;
      this.city = null;
      this.vehicles = new Map();   // id -> latest record from /vehicles
      this.samples = new Map();    // id -> [{t, lat, lng, offset}]
      this.headings = new Map();   // id -> last heading (radians, canvas space)
      this.view = null;
      this.zoom = 1;               // user zoom on top of the fitted view
      this.focus = null;           // {lat, lng} the user panned/zoomed to
      this.layer = null;           // cached street layer
      this.layerKey = "";
      this.points = [];
      this.hoverId = null;
      this.mouse = null;
      this.drag = null;

      new ResizeObserver(() => { this.layerKey = ""; }).observe(canvas);
      this.attachControls();

      fetch("/dashboard/city_map.json", { cache: "no-store" })
        .then((response) => (response.ok ? response.json() : null))
        .then((city) => { this.city = city; this.layerKey = ""; })
        .catch(() => { /* No street map: vans are drawn on a plain grid. */ });

      const frame = () => { this.draw(); requestAnimationFrame(frame); };
      requestAnimationFrame(frame);
    }

    attachControls() {
      const canvas = this.canvas;
      const local = (event) => {
        const rect = canvas.getBoundingClientRect();
        return { x: event.clientX - rect.left, y: event.clientY - rect.top };
      };
      canvas.addEventListener("mousemove", (event) => {
        this.mouse = local(event);
        if (this.drag && this.view) {
          const dx = this.mouse.x - this.drag.x, dy = this.mouse.y - this.drag.y;
          const ppd = this.view.pixelsPerDegree;
          this.focus = {
            lat: this.drag.lat + dy / ppd,
            lng: this.drag.lng - dx / (ppd * this.view.lngScale),
          };
          canvas.style.cursor = "grabbing";
        }
      });
      canvas.addEventListener("mousedown", (event) => {
        if (!this.view || event.button !== 0) return;
        const p = local(event);
        this.drag = { x: p.x, y: p.y, lat: this.view.centerLat, lng: this.view.centerLng };
      });
      window.addEventListener("mouseup", () => { this.drag = null; canvas.style.cursor = ""; });
      canvas.addEventListener("mouseleave", () => { this.mouse = null; });
      canvas.addEventListener("wheel", (event) => {
        if (!this.view) return;
        event.preventDefault();
        const p = local(event);
        const width = canvas.clientWidth, height = canvas.clientHeight;
        // Keep the point under the cursor fixed while zooming.
        const before = this.unproject(p.x, p.y, width, height);
        // One mouse-wheel notch zooms by ZOOM_STEP. Browsers report a notch
        // as lines (Firefox) or ~100 px (Chrome); trackpads send many small
        // pixel deltas, which zoom continuously at the same rate.
        const notch = event.deltaMode !== 0 || Math.abs(event.deltaY) >= 50;
        const steps = notch ? -Math.sign(event.deltaY) : -event.deltaY / 100;
        const factor = Math.pow(ZOOM_STEP, steps);
        const zoom = Math.min(MAX_ZOOM, Math.max(1, this.zoom * factor));
        const scale = zoom / this.zoom;
        this.zoom = zoom;
        if (zoom === 1) { this.focus = null; return; }
        const ppd = this.view.pixelsPerDegree * scale;
        this.focus = {
          lat: before.lat + (p.y - height / 2) / ppd,
          lng: before.lng - (p.x - width / 2) / (ppd * this.view.lngScale),
        };
      }, { passive: false });
      canvas.addEventListener("dblclick", () => { this.zoom = 1; this.focus = null; });
    }

    // Called with each /vehicles response; receivedAt is performance.now()
    // when the response arrived.
    update(vehicles, receivedAt) {
      const seen = new Set();
      for (const vehicle of vehicles) {
        const id = vehicle.vehicle_id;
        seen.add(id);
        this.vehicles.set(id, vehicle);
        const history = this.samples.get(id) || [];
        const last = history[history.length - 1];
        if (!last || last.offset !== vehicle.offset) {
          let t = receivedAt - vehicle.age_seconds * 1000;
          if (last && t <= last.t) t = last.t + 1;
          history.push({ t, lat: vehicle.lat, lng: vehicle.lng, offset: vehicle.offset });
        }
        while (history.length > 2 && history[1].t < receivedAt - HISTORY_MS) history.shift();
        this.samples.set(id, history);
      }
      for (const id of [...this.samples.keys()]) {
        if (!seen.has(id)) {
          this.samples.delete(id);
          this.vehicles.delete(id);
          this.headings.delete(id);
        }
      }
      this.emptyMessage.hidden = vehicles.length > 0;
    }

    // Position of a vehicle at time t, interpolated between reports.
    positionAt(history, t) {
      if (t <= history[0].t) return history[0];
      for (let i = history.length - 1; i >= 0; i -= 1) {
        const a = history[i];
        if (a.t > t) continue;
        const b = history[i + 1];
        if (!b) return a;                          // waiting for the next report
        if (b.t - a.t > SNAP_GAP_MS) return a;     // signal gap: no gliding
        const f = (t - a.t) / (b.t - a.t);
        return { lat: a.lat + (b.lat - a.lat) * f, lng: a.lng + (b.lng - a.lng) * f };
      }
      return history[0];
    }

    // --- view --------------------------------------------------------------

    // Fit the city (and any vehicle outside it), keeping equal metres per
    // pixel on both axes by scaling longitude with cos(latitude), then apply
    // the user's zoom and pan. Without user input the view never moves, so
    // the streets stay still.
    computeView(width, height) {
      let minLat = Infinity, maxLat = -Infinity, minLng = Infinity, maxLng = -Infinity;
      const include = (lat, lng) => {
        minLat = Math.min(minLat, lat); maxLat = Math.max(maxLat, lat);
        minLng = Math.min(minLng, lng); maxLng = Math.max(maxLng, lng);
      };
      if (this.city) {
        const b = this.city.bounds;
        include(b.min_lat, b.min_lng);
        include(b.max_lat, b.max_lng);
      }
      for (const v of this.vehicles.values()) include(v.lat, v.lng);
      if (!isFinite(minLat)) return null;
      const lngScale = Math.max(Math.cos((minLat + maxLat) / 2 * Math.PI / 180), 0.05);
      const pad = this.city ? 1.03 : 1.15;
      const spanLat = Math.max(maxLat - minLat, MIN_SPAN_DEGREES) * pad;
      const spanLng = Math.max((maxLng - minLng) * lngScale, MIN_SPAN_DEGREES) * pad;
      const fit = {
        centerLat: (minLat + maxLat) / 2,
        centerLng: (minLng + maxLng) / 2,
        lngScale,
        pixelsPerDegree: Math.min(width / spanLng, height / spanLat),
      };
      if (this.focus) {
        return { ...fit, centerLat: this.focus.lat, centerLng: this.focus.lng,
          pixelsPerDegree: fit.pixelsPerDegree * this.zoom };
      }
      if (!this.view || this.city) return fit;
      // Without a street map the fitted area follows the fleet: ease toward
      // it instead of jumping.
      const blend = (a, b) => a + (b - a) * 0.08;
      return { ...fit,
        centerLat: blend(this.view.centerLat, fit.centerLat),
        centerLng: blend(this.view.centerLng, fit.centerLng),
        pixelsPerDegree: blend(this.view.pixelsPerDegree, fit.pixelsPerDegree) };
    }

    project(lat, lng, width, height) {
      const view = this.view;
      return {
        x: width / 2 + (lng - view.centerLng) * view.lngScale * view.pixelsPerDegree,
        y: height / 2 - (lat - view.centerLat) * view.pixelsPerDegree,
      };
    }

    unproject(x, y, width, height) {
      const view = this.view;
      return {
        lat: view.centerLat - (y - height / 2) / view.pixelsPerDegree,
        lng: view.centerLng + (x - width / 2) / (view.lngScale * view.pixelsPerDegree),
      };
    }

    // --- drawing -------------------------------------------------------------

    draw() {
      const ratio = window.devicePixelRatio || 1;
      const width = this.canvas.clientWidth;
      const height = this.canvas.clientHeight;
      if (width === 0 || height === 0) return;
      if (this.canvas.width !== Math.round(width * ratio)) this.canvas.width = Math.round(width * ratio);
      if (this.canvas.height !== Math.round(height * ratio)) this.canvas.height = Math.round(height * ratio);
      const ctx = this.context;
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);

      this.view = this.computeView(width, height);
      this.points = [];
      if (!this.view) {
        ctx.fillStyle = MAP_STYLE.land;
        ctx.fillRect(0, 0, width, height);
        return;
      }

      const key = [width, height, ratio, this.city ? 1 : 0, this.view.centerLat.toFixed(7),
        this.view.centerLng.toFixed(7), this.view.pixelsPerDegree.toFixed(0)].join("|");
      if (key !== this.layerKey) {
        this.layer = this.renderLayer(width, height, ratio);
        this.layerKey = key;
      }
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.drawImage(this.layer, 0, 0);
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);

      const renderTime = performance.now() - RENDER_DELAY_MS;
      const metresToPixels = this.view.pixelsPerDegree / METRES_PER_DEGREE;
      const margin = 30;
      const drawn = [];
      for (const [id, history] of this.samples) {
        const vehicle = this.vehicles.get(id);
        const here = this.positionAt(history, renderTime);
        const p = this.project(here.lat, here.lng, width, height);
        if (p.x < -margin || p.y < -margin || p.x > width + margin || p.y > height + margin) continue;
        const ahead = this.positionAt(history, renderTime + 250);
        const q = this.project(ahead.lat, ahead.lng, width, height);
        if (Math.hypot(q.x - p.x, q.y - p.y) > 0.02) this.headings.set(id, Math.atan2(q.y - p.y, q.x - p.x));
        drawn.push({ id, vehicle, history, x: p.x, y: p.y, heading: this.headings.get(id) });
      }
      this.drawTrails(ctx, width, height, renderTime, drawn);

      // Offline vans underneath, then idle, then moving on top.
      const order = { offline: 0, idle: 1, moving: 2 };
      drawn.sort((a, b) => (order[a.vehicle.status] ?? 0) - (order[b.vehicle.status] ?? 0));
      // Vans are drawn larger than life (a van is ~6 m) so they stay readable.
      const size = Math.max(4, Math.min(11, 18 * metresToPixels));
      for (const item of drawn) {
        this.drawVan(ctx, item, size, item.id === this.hoverId);
        this.points.push(item);
      }
      this.drawScale(ctx, height);
      this.updateHover();
    }

    renderLayer(width, height, ratio) {
      const layer = document.createElement("canvas");
      layer.width = Math.round(width * ratio);
      layer.height = Math.round(height * ratio);
      const ctx = layer.getContext("2d");
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      ctx.fillStyle = MAP_STYLE.land;
      ctx.fillRect(0, 0, width, height);
      if (!this.city) {
        this.drawGrid(ctx, width, height);
        return layer;
      }
      const city = this.city;
      const metresToPixels = this.view.pixelsPerDegree / METRES_PER_DEGREE;
      const toPixel = ([lat, lng]) => this.project(lat, lng, width, height);

      for (const area of city.areas) {
        ctx.beginPath();
        area.points.forEach((point, index) => {
          const p = toPixel(point);
          if (index === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y);
        });
        ctx.closePath();
        ctx.fillStyle = area.kind === "water" ? MAP_STYLE.water : MAP_STYLE.park;
        ctx.fill();
      }

      const roadWidth = (cls) => Math.max(cls === "major" ? 2.5 : 1.2,
        (cls === "major" ? 16 : 10) * metresToPixels);
      const strokeRoads = (roads, lineWidth, color) => {
        ctx.beginPath();
        for (const road of roads) {
          road.nodes.forEach((node, index) => {
            const p = toPixel(city.nodes[node]);
            if (index === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y);
          });
        }
        ctx.lineWidth = lineWidth;
        ctx.strokeStyle = color;
        ctx.stroke();
      };
      const major = city.roads.filter((r) => r.class === "major");
      const minor = city.roads.filter((r) => r.class !== "major");
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      strokeRoads(minor, roadWidth("minor") + 2, MAP_STYLE.casing);
      strokeRoads(major, roadWidth("major") + 2, MAP_STYLE.casing);
      strokeRoads(minor, roadWidth("minor"), MAP_STYLE.minor);
      // Main roads on top so they read as continuous through junctions.
      strokeRoads(major, roadWidth("major"), MAP_STYLE.major);

      // Labels in priority order; each is skipped if it would overlap one
      // already placed.
      this.labelBoxes = [];
      const depots = city.depots || (city.depot ? [city.depot] : []);
      for (const depot of depots) {
        const p = toPixel(city.nodes[depot.node]);
        ctx.fillStyle = "#4aa3ff";
        ctx.strokeStyle = MAP_STYLE.casing;
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.rect(p.x - 6, p.y - 6, 12, 12);
        ctx.fill();
        ctx.stroke();
        ctx.font = "600 11px system-ui, sans-serif";
        ctx.textAlign = "left";
        ctx.textBaseline = "middle";
        const textWidth = ctx.measureText(depot.name).width;
        this.claim(p.x - 7, p.y - 17, textWidth + 18, 24);
        this.haloText(ctx, depot.name, p.x + 10, p.y - 10, "#8fc4ff");
      }
      for (const district of city.districts || []) {
        const p = toPixel(district.at);
        ctx.font = `600 ${Math.round(Math.min(22, 12 + this.zoom * 2))}px system-ui, sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        const name = district.name.toUpperCase();
        const w = ctx.measureText(name).width;
        if (!this.claim(p.x - w / 2, p.y - 9, w, 18)) continue;
        ctx.fillStyle = MAP_STYLE.district;
        ctx.fillText(name, p.x, p.y);
      }
      for (const area of city.areas) this.drawAreaLabel(ctx, area, toPixel);
      this.drawRoadLabels(ctx, toPixel);
      return layer;
    }

    // Reserve a screen rectangle for a label; false if it is already taken.
    claim(x, y, w, h) {
      const pad = 3;
      for (const b of this.labelBoxes) {
        if (x < b.x + b.w + pad && x + w + pad > b.x && y < b.y + b.h + pad && y + h + pad > b.y) {
          return false;
        }
      }
      this.labelBoxes.push({ x, y, w, h });
      return true;
    }

    // Label each street once, along its longest stretch that is nearly
    // straight, when the name fits there at the current zoom.
    drawRoadLabels(ctx, toPixel) {
      const width = this.canvas.clientWidth, height = this.canvas.clientHeight;
      const best = new Map();
      for (const road of this.city.roads) {
        const pts = road.nodes.map((node) => toPixel(this.city.nodes[node]));
        for (let i = 0; i + 1 < pts.length; i += 1) {
          // Extend the stretch while the direction stays within ~15 degrees.
          const angle0 = Math.atan2(pts[i + 1].y - pts[i].y, pts[i + 1].x - pts[i].x);
          let j = i + 1;
          while (j + 1 < pts.length) {
            const angle = Math.atan2(pts[j + 1].y - pts[j].y, pts[j + 1].x - pts[j].x);
            if (Math.abs(((angle - angle0 + 3 * Math.PI) % (2 * Math.PI)) - Math.PI) > 0.26) break;
            j += 1;
          }
          // Only the visible part of the stretch can carry the label.
          const a = clip(pts[i], pts[j], width, height), b = clip(pts[j], pts[i], width, height);
          if (!a || !b) continue;
          const length = Math.hypot(b.x - a.x, b.y - a.y);
          const current = best.get(road.name);
          if (!current || length > current.length) best.set(road.name, { a, b, length, road });
        }
      }
      ctx.font = "10px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      const ranked = [...best.entries()].sort(([, x], [, y]) =>
        (y.road.class === "major") - (x.road.class === "major") || y.length - x.length);
      for (const [name, { a, b, length, road }] of ranked) {
        const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
        if (mx < 0 || my < 0 || mx > width || my > height) continue;
        const textWidth = ctx.measureText(name).width;
        if (textWidth + 24 > length) continue;
        // Screen box around the (possibly rotated) label.
        const ux = Math.abs(b.x - a.x) / length, uy = Math.abs(b.y - a.y) / length;
        const boxW = textWidth * ux + 12 * uy, boxH = textWidth * uy + 12 * ux;
        if (!this.claim(mx - boxW / 2, my - boxH / 2, boxW, boxH)) continue;
        let angle = Math.atan2(b.y - a.y, b.x - a.x);
        if (angle > Math.PI / 2) angle -= Math.PI;
        if (angle < -Math.PI / 2) angle += Math.PI;
        ctx.save();
        ctx.translate(mx, my);
        ctx.rotate(angle);
        this.haloText(ctx, name, 0, 0, road.class === "major" ? "#a7b3c1" : MAP_STYLE.label);
        ctx.restore();
      }
    }

    drawAreaLabel(ctx, area, toPixel) {
      const pts = area.points.map(toPixel);
      let x = 0, y = 0;
      for (const p of pts) { x += p.x; y += p.y; }
      x /= pts.length; y /= pts.length;
      ctx.font = "italic 11px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      if (area.kind === "park") {
        let minX = Infinity, maxX = -Infinity;
        for (const p of pts) { minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x); }
        if (ctx.measureText(area.name).width + 6 > maxX - minX) return;
      } else {
        // A long river: label it near the middle of what is on screen.
        const width = this.canvas.clientWidth;
        const half = pts.length / 2;
        let bestIndex = 0, bestDistance = Infinity;
        for (let i = 0; i < half; i += 1) {
          const distance = Math.abs(pts[i].x - width / 2);
          if (distance < bestDistance) { bestDistance = distance; bestIndex = i; }
        }
        const top = pts[bestIndex], bottom = pts[pts.length - 1 - bestIndex];
        x = (top.x + bottom.x) / 2;
        y = (top.y + bottom.y) / 2;
      }
      const textWidth = ctx.measureText(area.name).width;
      if (!this.claim(x - textWidth / 2, y - 7, textWidth, 14)) return;
      ctx.fillStyle = area.kind === "water" ? MAP_STYLE.waterLabel : MAP_STYLE.parkLabel;
      ctx.fillText(area.name, x, y);
    }

    haloText(ctx, text, x, y, color) {
      ctx.lineWidth = 3;
      ctx.strokeStyle = MAP_STYLE.land;
      ctx.strokeText(text, x, y);
      ctx.fillStyle = color;
      ctx.fillText(text, x, y);
    }

    drawGrid(ctx, width, height) {
      ctx.strokeStyle = "#1b232d";
      ctx.lineWidth = 1;
      const step = 48;
      ctx.beginPath();
      for (let x = (width % step) / 2; x < width; x += step) { ctx.moveTo(x, 0); ctx.lineTo(x, height); }
      for (let y = (height % step) / 2; y < height; y += step) { ctx.moveTo(0, y); ctx.lineTo(width, y); }
      ctx.stroke();
    }

    // Recent path of each van. All trails of one colour go into a single
    // path, so a thousand vans cost three strokes per frame.
    drawTrails(ctx, width, height, renderTime, drawn) {
      const paths = {};
      for (const item of drawn) {
        const status = item.vehicle.status;
        if (status === "offline") continue;
        const path = paths[status] || (paths[status] = new Path2D());
        let started = false;
        for (const sample of item.history) {
          if (sample.t < renderTime - TRAIL_MS || sample.t > renderTime) continue;
          const p = this.project(sample.lat, sample.lng, width, height);
          if (started) path.lineTo(p.x, p.y); else { path.moveTo(p.x, p.y); started = true; }
        }
        if (started) path.lineTo(item.x, item.y);
      }
      ctx.lineCap = "round";
      ctx.lineJoin = "round";
      ctx.lineWidth = 2;
      ctx.globalAlpha = 0.3;
      for (const [status, path] of Object.entries(paths)) {
        ctx.strokeStyle = COLORS[status] || "#8b98a8";
        ctx.stroke(path);
      }
      ctx.globalAlpha = 1;
    }

    // A van is an arrow pointing the way it is driving.
    drawVan(ctx, item, size, highlighted) {
      const status = item.vehicle.status;
      const color = COLORS[status] || "#8b98a8";
      ctx.save();
      ctx.translate(item.x, item.y);
      if (highlighted) {
        ctx.beginPath();
        ctx.arc(0, 0, Math.max(10, size * 2), 0, Math.PI * 2);
        ctx.fillStyle = color + "33";
        ctx.fill();
      }
      ctx.globalAlpha = status === "offline" ? 0.75 : 1;
      if (item.heading === undefined || status === "idle") {
        ctx.beginPath();
        ctx.arc(0, 0, size * 0.75, 0, Math.PI * 2);
      } else {
        ctx.rotate(item.heading);
        ctx.beginPath();
        ctx.moveTo(size * 1.1, 0);
        ctx.lineTo(-size * 0.8, size * 0.75);
        ctx.lineTo(-size * 0.4, 0);
        ctx.lineTo(-size * 0.8, -size * 0.75);
        ctx.closePath();
      }
      ctx.fillStyle = color;
      ctx.fill();
      ctx.lineWidth = size < 6 ? 1 : 1.5;
      ctx.strokeStyle = MAP_STYLE.casing;
      ctx.stroke();
      ctx.restore();
    }

    drawScale(ctx, height) {
      const pixelsPerKm = this.view.pixelsPerDegree / (METRES_PER_DEGREE / 1000);
      const km = this.pickScale(pixelsPerKm);
      ctx.fillStyle = "#8b98a8";
      ctx.fillRect(16, height - 18, km * pixelsPerKm, 3);
      ctx.font = "11px system-ui, sans-serif";
      ctx.textAlign = "left";
      ctx.textBaseline = "alphabetic";
      ctx.fillText(km >= 1 ? `${km} km` : `${km * 1000} m`, 16, height - 24);
    }

    pickScale(pixelsPerKm) {
      const options = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500];
      for (const km of options) if (km * pixelsPerKm >= 60) return km;
      return options[options.length - 1];
    }

    // Tooltip follows the hovered van as it moves.
    updateHover() {
      let best = null;
      if (this.mouse && !this.drag) {
        let bestDistance = 14;
        for (const point of this.points) {
          const distance = Math.hypot(point.x - this.mouse.x, point.y - this.mouse.y);
          if (distance < bestDistance) { best = point; bestDistance = distance; }
        }
      }
      this.hoverId = best ? best.id : null;
      if (!best) { this.tooltip.hidden = true; return; }
      const v = best.vehicle;
      this.tooltip.innerHTML =
        `<strong>${escapeHtml(v.vehicle_id)}</strong><br>` +
        `${escapeHtml(v.status)} · ${v.speed.toFixed(1)} km/h<br>` +
        `${v.lat.toFixed(5)}, ${v.lng.toFixed(5)}<br>` +
        `last report ${v.age_seconds.toFixed(0)} s ago`;
      this.tooltip.hidden = false;
      const width = this.canvas.clientWidth;
      const left = Math.min(best.x + 14, width - this.tooltip.offsetWidth - 4);
      this.tooltip.style.left = `${Math.max(4, left)}px`;
      this.tooltip.style.top = `${Math.max(4, best.y - 10)}px`;
    }
  }

  // Move point p toward q until it is inside the canvas; null if the segment
  // p-q never enters it.
  function clip(p, q, width, height) {
    let lo = 0, hi = 1;
    for (const [d, v, max] of [[q.x - p.x, p.x, width], [q.y - p.y, p.y, height]]) {
      if (d === 0) {
        if (v < 0 || v > max) return null;
        continue;
      }
      const t0 = (0 - v) / d, t1 = (max - v) / d;
      lo = Math.max(lo, Math.min(t0, t1));
      hi = Math.min(hi, Math.max(t0, t1));
    }
    if (lo > hi) return null;
    return { x: p.x + (q.x - p.x) * lo, y: p.y + (q.y - p.y) * lo };
  }

  function escapeHtml(text) {
    return String(text).replace(/[&<>"']/g, (c) => (
      { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  window.FleetMap = FleetMap;
  window.escapeHtml = escapeHtml;
})();
