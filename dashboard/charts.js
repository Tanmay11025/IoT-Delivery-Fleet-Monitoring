// Rolling line charts built on Chart.js.
//
// The history lives only in the browser (the last WINDOW polls); the gateway
// exposes current counters, not time series. If the Chart.js CDN cannot be
// reached the dashboard keeps working without the two charts.
(function () {
  "use strict";

  const WINDOW = 120;

  class RollingChart {
    constructor(canvas, label, color) {
      this.labels = [];
      this.values = [];
      this.chart = null;
      if (typeof window.Chart === "undefined") {
        const note = document.createElement("div");
        note.className = "empty";
        note.textContent = "Chart.js could not be loaded (offline?)";
        canvas.parentElement.appendChild(note);
        return;
      }
      this.chart = new window.Chart(canvas, {
        type: "line",
        data: {
          labels: this.labels,
          datasets: [{
            label,
            data: this.values,
            borderColor: color,
            backgroundColor: color + "22",
            borderWidth: 2,
            fill: true,
            pointRadius: 0,
            tension: 0.25,
          }],
        },
        options: {
          animation: false,
          responsive: true,
          maintainAspectRatio: false,
          interaction: { intersect: false, mode: "index" },
          plugins: { legend: { display: false } },
          scales: {
            x: { ticks: { color: "#8b98a8", maxTicksLimit: 6 }, grid: { color: "#1b232d" } },
            y: { beginAtZero: true, ticks: { color: "#8b98a8", precision: 0 }, grid: { color: "#1b232d" } },
          },
        },
      });
    }

    push(value) {
      const now = new Date();
      this.labels.push(now.toLocaleTimeString([], { hour12: false }));
      this.values.push(value);
      if (this.labels.length > WINDOW) {
        this.labels.shift();
        this.values.shift();
      }
      if (this.chart) this.chart.update("none");
    }
  }

  window.RollingChart = RollingChart;
})();
