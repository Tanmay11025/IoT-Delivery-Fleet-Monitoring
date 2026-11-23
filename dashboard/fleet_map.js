// Canvas fleet map: plots each vehicle's last known position.
//
// No map tiles or API key: positions are projected onto a plain grid that
// auto-fits to the fleet's bounding box. Short client-side trails show
// direction of travel.
(function () {
  "use strict";

  const COLORS = { moving: "#2ecc71", idle: "#f1c40f", offline: "#e5534b" };
  const TRAIL_LENGTH = 15;
  const MIN_SPAN_DEGREES = 0.01;

  class FleetMap {
    constructor(canvas, tooltip, emptyMessage) {
      this.canvas = canvas;
      this.context = canvas.getContext("2d");
      this.tooltip = tooltip;
      this.emptyMessage = emptyMessage;
      this.vehicles = [];
      this.trails = new Map();
      this.view = null;
      this.points = [];

      new ResizeObserver(() => this.draw()).observe(canvas);
      canvas.addEventListener("mousemove", (event) => this.hover(event));
      canvas.addEventListener("mouseleave", () => { this.tooltip.hidden = true; });
    }

    update(vehicles) {
      this.vehicles = vehicles;
      const seen = new Set();
      for (const vehicle of vehicles) {
        seen.add(vehicle.vehicle_id);
        const trail = this.trails.get(vehicle.vehicle_id) || [];
        const last = trail[trail.length - 1];
        if (!last || last.lat !== vehicle.lat || last.lng !== vehicle.lng) {
          trail.push({ lat: vehicle.lat, lng: vehicle.lng });
          if (trail.length > TRAIL_LENGTH) trail.shift();
        }
        this.trails.set(vehicle.vehicle_id, trail);
      }
      for (const id of this.trails.keys()) if (!seen.has(id)) this.trails.delete(id);
      this.emptyMessage.hidden = vehicles.length > 0;
      this.draw();
    }

    // Fit the bounding box, keeping roughly equal metres per pixel on both
    // axes by scaling longitude with cos(latitude).
    computeView(width, height) {
      let minLat = Infinity, maxLat = -Infinity, minLng = Infinity, maxLng = -Infinity;
      for (const trail of this.trails.values()) {
        for (const point of trail) {
          minLat = Math.min(minLat, point.lat); maxLat = Math.max(maxLat, point.lat);
          minLng = Math.min(minLng, point.lng); maxLng = Math.max(maxLng, point.lng);
        }
      }
      if (!isFinite(minLat)) return null;
      const centerLat = (minLat + maxLat) / 2;
      const centerLng = (minLng + maxLng) / 2;
      const lngScale = Math.max(Math.cos(centerLat * Math.PI / 180), 0.05);
      const spanLat = Math.max(maxLat - minLat, MIN_SPAN_DEGREES) * 1.15;
      const spanLng = Math.max((maxLng - minLng) * lngScale, MIN_SPAN_DEGREES) * 1.15;
      const pixelsPerDegree = Math.min(width / spanLng, height / spanLat);
      // Smooth zoom changes so the map does not jump every second.
      const target = { centerLat, centerLng, lngScale, pixelsPerDegree };
      if (!this.view) return target;
      const blend = (a, b) => a + (b - a) * 0.25;
      return {
        centerLat: blend(this.view.centerLat, target.centerLat),
        centerLng: blend(this.view.centerLng, target.centerLng),
        lngScale: target.lngScale,
        pixelsPerDegree: blend(this.view.pixelsPerDegree, target.pixelsPerDegree),
      };
    }

    project(lat, lng, width, height) {
      const view = this.view;
      return {
        x: width / 2 + (lng - view.centerLng) * view.lngScale * view.pixelsPerDegree,
        y: height / 2 - (lat - view.centerLat) * view.pixelsPerDegree,
      };
    }

    draw() {
      const ratio = window.devicePixelRatio || 1;
      const width = this.canvas.clientWidth;
      const height = this.canvas.clientHeight;
      if (width === 0 || height === 0) return;
      if (this.canvas.width !== Math.round(width * ratio)) this.canvas.width = Math.round(width * ratio);
      if (this.canvas.height !== Math.round(height * ratio)) this.canvas.height = Math.round(height * ratio);
      const ctx = this.context;
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      ctx.clearRect(0, 0, width, height);

      this.view = this.computeView(width, height);
      this.drawGrid(ctx, width, height);
      this.points = [];
      if (!this.view) return;

      for (const [id, trail] of this.trails) {
        if (trail.length < 2) continue;
        const vehicle = this.vehicles.find((v) => v.vehicle_id === id);
        ctx.strokeStyle = (COLORS[vehicle && vehicle.status] || "#8b98a8") + "55";
        ctx.lineWidth = 2;
        ctx.beginPath();
        trail.forEach((point, index) => {
          const p = this.project(point.lat, point.lng, width, height);
          if (index === 0) ctx.moveTo(p.x, p.y); else ctx.lineTo(p.x, p.y);
        });
        ctx.stroke();
      }

      for (const vehicle of this.vehicles) {
        const p = this.project(vehicle.lat, vehicle.lng, width, height);
        const color = COLORS[vehicle.status] || "#8b98a8";
        ctx.beginPath();
        ctx.arc(p.x, p.y, 6, 0, Math.PI * 2);
        ctx.fillStyle = color;
        ctx.fill();
        ctx.lineWidth = 2;
        ctx.strokeStyle = "#0c1116";
        ctx.stroke();
        this.points.push({ x: p.x, y: p.y, vehicle });
      }
    }

    drawGrid(ctx, width, height) {
      ctx.strokeStyle = "#1b232d";
      ctx.lineWidth = 1;
      const step = 48;
      ctx.beginPath();
      for (let x = (width % step) / 2; x < width; x += step) { ctx.moveTo(x, 0); ctx.lineTo(x, height); }
      for (let y = (height % step) / 2; y < height; y += step) { ctx.moveTo(0, y); ctx.lineTo(width, y); }
      ctx.stroke();
      if (!this.view) return;
      // Scale bar: 1 degree of latitude is ~111 km.
      const km = this.pickScale(this.view.pixelsPerDegree / 111.0);
      const pixels = km * this.view.pixelsPerDegree / 111.0;
      ctx.fillStyle = "#8b98a8";
      ctx.fillRect(16, height - 18, pixels, 3);
      ctx.font = "11px system-ui, sans-serif";
      ctx.fillText(km >= 1 ? `${km} km` : `${km * 1000} m`, 16, height - 24);
    }

    pickScale(pixelsPerKm) {
      const options = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500];
      for (const km of options) if (km * pixelsPerKm >= 60) return km;
      return options[options.length - 1];
    }

    hover(event) {
      const rect = this.canvas.getBoundingClientRect();
      const x = event.clientX - rect.left;
      const y = event.clientY - rect.top;
      let best = null, bestDistance = 12;
      for (const point of this.points) {
        const distance = Math.hypot(point.x - x, point.y - y);
        if (distance < bestDistance) { best = point; bestDistance = distance; }
      }
      if (!best) { this.tooltip.hidden = true; return; }
      const v = best.vehicle;
      this.tooltip.innerHTML =
        `<strong>${escapeHtml(v.vehicle_id)}</strong><br>` +
        `${escapeHtml(v.status)} · ${v.speed.toFixed(1)} km/h<br>` +
        `${v.lat.toFixed(5)}, ${v.lng.toFixed(5)}<br>` +
        `seen ${v.age_seconds.toFixed(0)} s ago`;
      this.tooltip.hidden = false;
      const left = Math.min(best.x + 12, rect.width - this.tooltip.offsetWidth - 4);
      this.tooltip.style.left = `${Math.max(4, left)}px`;
      this.tooltip.style.top = `${Math.max(4, best.y - 10)}px`;
    }
  }

  function escapeHtml(text) {
    return String(text).replace(/[&<>"']/g, (c) => (
      { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  window.FleetMap = FleetMap;
  window.escapeHtml = escapeHtml;
})();
