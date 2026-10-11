// WadiWatch dashboard: the curves of each wadi node, node health, and the age
// of the gateway's latest data. Polls the health and readings APIs on its own;
// the map and the node list are dashboard.js.
"use strict";

(() => {
  const HEALTH_URL = "api/health";
  const readingsUrl = (nodeId, from, to) => `api/nodes/${nodeId}/readings?from=${from}&to=${to}`;
  const POLL_MS = 2000;

  // Periods the officer can choose for the curves, in seconds.
  // The shortest is for a live demonstration and a replay, where an event lasts minutes.
  const PERIODS = [
    { seconds: 5 * 60, label: "5 min" },
    { seconds: 30 * 60, label: "30 min" },
    { seconds: 2 * 3600, label: "2 h" },
    { seconds: 6 * 3600, label: "6 h" },
    { seconds: 24 * 3600, label: "24 h" },
  ];
  const DEFAULT_PERIOD = PERIODS.find((choice) => choice.label === "2 h");
  const ROLE_LABELS = { wadi_node: "Wadi node", siren_node: "Siren node" };
  const SVG_NS = "http://www.w3.org/2000/svg";

  const gatewayStatus = document.getElementById("gateway-status");
  const gatewayTitle = document.getElementById("gateway-status-title");
  const gatewayText = document.getElementById("gateway-status-text");
  const gatewayAnnouncement = document.getElementById("gateway-announcement");
  const healthFlags = document.getElementById("health-flags");
  const healthRows = document.getElementById("health-rows");
  const healthRule = document.getElementById("health-rule");
  const curves = document.getElementById("curves");
  const periodButtons = document.getElementById("curve-periods");

  let period = DEFAULT_PERIOD;
  let pollTimer = null;
  // Counts refreshes, so one that was overtaken by a newer one can tell.
  let refreshes = 0;
  let lastUpdate = null;
  // What the curves were last drawn from, kept to redraw on resize and hover.
  let drawn = { nodes: [], series: [] };
  // Gateway time under the pointer, shared by every chart, or null.
  let hoverTs = null;

  function element(tag, className, text) {
    const made = document.createElement(tag);
    if (className) made.className = className;
    if (text !== undefined) made.textContent = text;
    return made;
  }

  function svg(tag, attributes, text) {
    const made = document.createElementNS(SVG_NS, tag);
    for (const [name, value] of Object.entries(attributes || {})) made.setAttribute(name, value);
    if (text !== undefined) made.textContent = text;
    return made;
  }

  // "45 s", "12 min", "2 h 5 min", "3 d 4 h"
  function duration(seconds) {
    if (seconds < 60) return `${seconds} s`;
    const minutes = Math.floor(seconds / 60);
    if (minutes < 60) return `${minutes} min`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return minutes % 60 ? `${hours} h ${minutes % 60} min` : `${hours} h`;
    const days = Math.floor(hours / 24);
    return hours % 24 ? `${days} d ${hours % 24} h` : `${days} d`;
  }

  function clockTime(ts, withSeconds) {
    const options = { hour12: false, hour: "2-digit", minute: "2-digit" };
    if (withSeconds) options.second = "2-digit";
    return new Date(ts * 1000).toLocaleTimeString([], options);
  }

  function dateAndTime(ts) {
    return new Date(ts * 1000).toLocaleString([], { hour12: false });
  }

  // Gateway status strip: the age of the latest data, or the stale-data warning.

  function showGatewayStatus(state, title, text) {
    // The age changes at every poll; a screen reader is told only when the state does.
    if (gatewayStatus.dataset.state !== state || gatewayAnnouncement.dataset.title !== title) {
      gatewayAnnouncement.textContent = state === "stale" ? `${title}. ${text}` : "Data from the gateway is current.";
      gatewayAnnouncement.dataset.title = title;
    }
    gatewayStatus.dataset.state = state;
    gatewayStatus.className = `gateway-status gateway-status--${state}`;
    gatewayTitle.hidden = !title;
    gatewayTitle.textContent = title || "";
    gatewayText.textContent = text;
    // Lets the rest of the page mark its picture as the last known one.
    document.body.classList.toggle("stale", state === "stale");
  }

  function showGateway(gateway) {
    if (gateway.age_s === null) {
      showGatewayStatus(
        "stale",
        "No data",
        "Nothing has been received from the gateway yet. The map shows no live picture of the wadi.",
      );
    } else if (gateway.stale) {
      showGatewayStatus(
        "stale",
        "Stale data",
        `The gateway has been silent for ${duration(gateway.age_s)}. The map, levels and curves `
          + "show the last known state, not what the wadi is doing now.",
      );
    } else {
      showGatewayStatus("fresh", "", `Latest data from the gateway: ${duration(gateway.age_s)} ago`);
    }
  }

  function showUnreachable() {
    const since = lastUpdate === null ? "" : ` since ${clockTime(lastUpdate / 1000, true)}`;
    showGatewayStatus(
      "stale",
      "Not updating",
      `The backend has not answered${since}. This page shows the last known state, `
        + "not what the wadi is doing now.",
    );
  }

  // Node health

  // The conditions a technician must act on, most serious first.
  function problems(node) {
    const found = [];
    if (node.faulty) found.push({ kind: "faulty", label: "Faulty" });
    if (node.low_battery) found.push({ kind: "low-battery", label: "Low battery" });
    if (node.tamper) found.push({ kind: "tamper", label: "Tamper" });
    if (node.refresh_lost) found.push({ kind: "refresh-lost", label: "Alert refresh lost" });
    return found;
  }

  function flag(problem) {
    return element("span", `flag flag--${problem.kind}`, problem.label);
  }

  function cell(label, className, ...content) {
    const made = element("td", className);
    made.dataset.label = label;
    made.append(...content);
    return made;
  }

  function heartbeatText(node) {
    if (node.heartbeat_ts === null) return node.faulty ? "Never received" : "None yet";
    return `${duration(node.heartbeat_age_s)} ago`;
  }

  function healthRow(node) {
    const found = problems(node);
    const row = element("tr", found.length ? "health__row health__row--attention" : "health__row");

    const name = element("th", "health__node");
    name.scope = "row";
    name.append(
      element("span", "health__name", node.name),
      element("span", "health__role", ROLE_LABELS[node.role] || node.role),
    );

    let status;
    if (found.length) status = found.map(flag);
    else if (node.heartbeat_ts === null) status = [element("span", "flag flag--waiting", "No heartbeat yet")];
    else status = [element("span", "flag flag--ok", "OK")];

    const none = "—";
    const heard = node.heartbeat_ts !== null;
    const heartbeat = cell(
      "Last heartbeat", node.faulty ? "health__value health__value--attention" : "health__value",
      heartbeatText(node),
    );
    if (heard) heartbeat.title = dateAndTime(node.heartbeat_ts);

    row.append(
      name,
      cell("Status", "health__status", ...status),
      cell(
        "Battery", node.low_battery ? "health__value health__value--attention" : "health__value",
        heard ? `${(node.battery_mv / 1000).toFixed(2)} V` : none,
      ),
      cell("Signal", "health__value", heard ? `${node.rssi_dbm} dBm` : none),
      cell(
        "Tamper switch", node.tamper ? "health__value health__value--attention" : "health__value",
        heard ? (node.tamper ? "Open" : "Closed") : none,
      ),
      cell("Discarded readings", "health__value", node.discards === null ? none : String(node.discards)),
      heartbeat,
    );
    return row;
  }

  function showHealth(health) {
    healthRows.replaceChildren(...health.nodes.map(healthRow));
    healthRule.textContent =
      `A heartbeat is expected every ${duration(health.heartbeat_interval_s)}. `
      + `A node is faulty when none has come for more than ${duration(health.faulty_after_s)}.`;

    // The same conditions next to the gateway status, where they are seen without scrolling.
    const flags = [];
    for (const node of health.nodes) {
      for (const problem of problems(node)) {
        const item = element("li");
        const link = element("a", `flag flag--${problem.kind}`);
        link.href = "#health";
        link.append(element("b", "", problem.label), ` ${node.name}`);
        item.append(link);
        flags.push(item);
      }
    }
    healthFlags.replaceChildren(...flags);
  }

  // Curves: one chart per wadi node, level above rate of rise on a shared time axis.

  const CHART = {
    left: 46, right: 26,
    levelTop: 24, levelHeight: 150,
    rateTop: 206, rateHeight: 86,
    height: 318,
  };
  const TIME_STEPS = [10, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200];
  // Shortest stretch of time a chart shows, so a single reading is not stretched across it.
  const MIN_SPAN_S = 300;

  // A step of 1, 2 or 5 times a power of ten that splits `span` into about `count` parts.
  function niceStep(span, count) {
    const rough = span / count;
    const magnitude = 10 ** Math.floor(Math.log10(rough));
    const factor = [1, 2, 5, 10].find((candidate) => candidate * magnitude >= rough);
    return factor * magnitude;
  }

  function niceScale(low, high, count) {
    const step = niceStep(high - low, count);
    const from = Math.floor(low / step) * step;
    const to = Math.ceil(high / step) * step;
    const ticks = [];
    for (let tick = from; tick <= to + step / 2; tick += step) ticks.push(Number(tick.toFixed(6)));
    return { from, to, ticks };
  }

  // The scales every chart shares, so the wadi nodes can be compared side by side.
  function sharedScales(series) {
    const readings = series.flatMap((one) => one.readings);
    const to = series[0].to;
    const first = readings.length ? Math.min(...readings.map((reading) => reading.ts)) : series[0].from;
    // The axis always ends now, so a silence shows as a gap on the right. It
    // starts where the data starts if that is later than the chosen period.
    const from = Math.max(series[0].from, Math.min(first, to - MIN_SPAN_S));
    const levels = readings.map((reading) => reading.level_cm);
    const rates = readings.map((reading) => reading.rise_cm_min);
    return {
      from,
      to,
      level: niceScale(0, Math.max(10, ...levels), 3),
      rate: niceScale(Math.min(0, ...rates), Math.max(1, ...rates), 2),
    };
  }

  function timeTicks(from, to, width) {
    const wanted = Math.max(2, Math.floor(width / 56));
    const step = TIME_STEPS.find((candidate) => (to - from) / candidate <= wanted) || 86400;
    // Whole steps of local time: the offset keeps hourly ticks on the hour.
    const offset = new Date(to * 1000).getTimezoneOffset() * 60;
    const ticks = [];
    for (let tick = Math.ceil((from - offset) / step) * step + offset; tick <= to; tick += step) {
      ticks.push(tick);
    }
    return { ticks, withSeconds: step < 60 };
  }

  function nearest(readings, ts) {
    let best = null;
    for (const reading of readings) {
      if (best === null || Math.abs(reading.ts - ts) < Math.abs(best.ts - ts)) best = reading;
    }
    return best;
  }

  function signed(value) {
    const text = Math.abs(value).toFixed(1);
    if (value > 0) return `+${text}`;
    return value < 0 ? `−${text}` : text;
  }

  function readout(card, reading, atPointer) {
    const target = card.querySelector(".curve__readout");
    if (!reading) {
      target.replaceChildren(element("span", "curve__empty", `No readings in the last ${period.label}`));
      return;
    }
    target.replaceChildren(
      element("span", "curve__when", `${atPointer ? "" : "Latest, "}${clockTime(reading.ts, true)}`),
      element("b", "", `${reading.level_cm.toFixed(1)} cm`),
      element("b", "", `${signed(reading.rise_cm_min)} cm/min`),
    );
  }

  function drawChart(card, node, series, scales) {
    const holder = card.querySelector(".curve__plot");
    const width = Math.max(260, Math.floor(holder.clientWidth));
    const plotWidth = width - CHART.left - CHART.right;
    const x = (ts) => CHART.left + ((ts - scales.from) / (scales.to - scales.from)) * plotWidth;
    const yIn = (scale, top, height) => (value) =>
      top + height - ((value - scale.from) / (scale.to - scale.from)) * height;
    const yLevel = yIn(scales.level, CHART.levelTop, CHART.levelHeight);
    const yRate = yIn(scales.rate, CHART.rateTop, CHART.rateHeight);
    const readings = series.readings.filter((reading) => reading.ts >= scales.from);
    const latest = readings[readings.length - 1];

    const chart = svg("svg", {
      class: "curve__svg", width, height: CHART.height, viewBox: `0 0 ${width} ${CHART.height}`,
      role: "img",
    });
    chart.setAttribute(
      "aria-label",
      latest
        ? `${node.name}: water level and rate of rise over the last ${period.label}. `
          + `Latest reading ${latest.level_cm.toFixed(1)} cm, ${signed(latest.rise_cm_min)} cm per minute.`
        : `${node.name}: no readings in the last ${period.label}.`,
    );

    const panels = [
      { title: "Water level, cm", scale: scales.level, y: yLevel, top: CHART.levelTop,
        height: CHART.levelHeight, value: (reading) => reading.level_cm, area: true },
      { title: "Rate of rise, cm/min", scale: scales.rate, y: yRate, top: CHART.rateTop,
        height: CHART.rateHeight, value: (reading) => reading.rise_cm_min, area: false },
    ];
    for (const panel of panels) {
      chart.append(svg("text", { class: "curve__title", x: CHART.left, y: panel.top - 9 }, panel.title));
      for (const tick of panel.scale.ticks) {
        const y = panel.y(tick);
        chart.append(
          svg("line", {
            class: tick === 0 ? "curve__grid curve__grid--zero" : "curve__grid",
            x1: CHART.left, x2: width - CHART.right, y1: y, y2: y,
          }),
          svg("text", { class: "curve__tick", x: CHART.left - 7, y: y + 4, "text-anchor": "end" }, String(tick)),
        );
      }
      if (readings.length === 0) continue;
      const points = readings.map((reading) => `${x(reading.ts).toFixed(1)},${panel.y(panel.value(reading)).toFixed(1)}`);
      if (panel.area) {
        const floor = panel.y(0);
        chart.append(svg("path", {
          class: "curve__area",
          d: `M${x(readings[0].ts).toFixed(1)},${floor} L${points.join(" L")} L${x(latest.ts).toFixed(1)},${floor} Z`,
        }));
      }
      chart.append(
        svg("path", { class: "curve__line", d: `M${points.join(" L")}` }),
        svg("circle", { class: "curve__end", cx: x(latest.ts), cy: panel.y(panel.value(latest)), r: 4 }),
      );
    }

    const time = timeTicks(scales.from, scales.to, plotWidth);
    const axisY = CHART.rateTop + CHART.rateHeight;
    for (const tick of time.ticks) {
      chart.append(
        svg("line", { class: "curve__grid", x1: x(tick), x2: x(tick), y1: axisY, y2: axisY + 5 }),
        svg("text", { class: "curve__tick", x: x(tick), y: axisY + 19, "text-anchor": "middle" },
          clockTime(tick, time.withSeconds)),
      );
    }

    // The reading nearest the pointer, marked on both curves.
    const hovered = hoverTs === null ? null : nearest(readings, hoverTs);
    if (hovered) {
      chart.append(
        svg("line", {
          class: "curve__crosshair", x1: x(hovered.ts), x2: x(hovered.ts),
          y1: CHART.levelTop, y2: axisY,
        }),
        svg("circle", { class: "curve__hover", cx: x(hovered.ts), cy: yLevel(hovered.level_cm), r: 4 }),
        svg("circle", { class: "curve__hover", cx: x(hovered.ts), cy: yRate(hovered.rise_cm_min), r: 4 }),
      );
    }
    readout(card, hovered || latest, Boolean(hovered));

    holder.replaceChildren(chart);
    // Where the pointer is in time, for the handlers on the holder.
    holder.tsAt = (clientX) => {
      const bounds = holder.getBoundingClientRect();
      const share = (clientX - bounds.left - CHART.left) / plotWidth;
      return scales.from + Math.min(1, Math.max(0, share)) * (scales.to - scales.from);
    };
    holder.readings = readings;
  }

  function curveCard(node) {
    const card = element("article", "curve");
    card.dataset.node = node.id;
    const head = element("header", "curve__head");
    head.append(element("h3", "curve__name", node.name), element("p", "curve__readout"));
    const holder = element("div", "curve__plot");
    const point = (event) => {
      if (!holder.tsAt || !holder.readings.length) return;
      // Snap to a reading of the chart under the pointer; the other charts follow it.
      hoverTs = nearest(holder.readings, holder.tsAt(event.clientX)).ts;
      drawCurves();
    };
    holder.addEventListener("pointermove", point);
    holder.addEventListener("pointerdown", point);
    holder.addEventListener("pointerleave", () => {
      hoverTs = null;
      drawCurves();
    });
    card.append(head, holder);
    return card;
  }

  function drawCurves() {
    const { nodes, series } = drawn;
    if (nodes.length === 0) {
      curves.replaceChildren(element("p", "curve__empty", "The site has no wadi node."));
      return;
    }
    // Cards are kept between polls so the pointer handlers and the hover survive.
    const wanted = nodes.map((node) => String(node.id)).join();
    if (curves.dataset.nodes !== wanted) {
      curves.replaceChildren(...nodes.map(curveCard));
      curves.dataset.nodes = wanted;
    }
    const scales = sharedScales(series);
    nodes.forEach((node, index) => drawChart(curves.children[index], node, series[index], scales));
  }

  function showPeriods() {
    periodButtons.replaceChildren(...PERIODS.map((choice) => {
      const button = element("button", "periods__choice", choice.label);
      button.type = "button";
      button.setAttribute("aria-pressed", String(choice === period));
      button.addEventListener("click", () => {
        if (choice === period) return;
        period = choice;
        hoverTs = null;
        showPeriods();
        refresh();
      });
      return button;
    }));
  }

  // Polling

  async function getJson(url) {
    const response = await fetch(url, { cache: "no-store" });
    if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
    return response.json();
  }

  async function refresh() {
    clearTimeout(pollTimer);
    const run = ++refreshes;
    const asked = period;
    try {
      const health = await getJson(HEALTH_URL);
      const nodes = health.nodes.filter((node) => node.role === "wadi_node");
      // The period ends at the backend's time, not this computer's.
      const series = await Promise.all(
        nodes.map((node) => getJson(readingsUrl(node.id, health.now - asked.seconds, health.now))),
      );
      // A period chosen while this was on its way has its own refresh.
      if (run !== refreshes) return;
      lastUpdate = Date.now();
      showGateway(health.gateway);
      showHealth(health);
      drawn = { nodes, series };
      drawCurves();
    } catch (error) {
      // A page that has stopped updating is stale too, and must say so.
      console.error(error);
      if (run === refreshes) showUnreachable();
    } finally {
      if (run === refreshes) pollTimer = setTimeout(refresh, POLL_MS);
    }
  }

  // Charts are drawn at the width they are given.
  new ResizeObserver(() => {
    if (drawn.series.length) drawCurves();
  }).observe(curves);

  showPeriods();
  refresh();
})();
