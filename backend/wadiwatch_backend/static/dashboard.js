// WadiWatch dashboard: polls the nodes API and colours each node by its alert level.
"use strict";

const NODES_URL = "api/nodes";
const POLL_MS = 2000;

// In order of severity. "silent" is a node the backend has never heard from.
const LEVEL_LABELS = {
  silent: "No data",
  none: "No alert",
  watch: "Watch",
  warning: "Warning",
  evacuate: "Evacuate",
};
const SEVERITY = ["none", "watch", "warning", "evacuate"];
const ROLE_LABELS = { wadi_node: "Wadi node", siren_node: "Siren node" };

const siteName = document.getElementById("site-name");
const summary = document.getElementById("summary");
const connection = document.getElementById("connection");
const nodeList = document.getElementById("node-list");
const placeholderNote = document.getElementById("placeholder-note");

// null until created, and for good if Leaflet could not be loaded.
let map = null;
let mapTried = false;
const markers = new Map();

function shownLevel(node) {
  if (node.last_message_ts === null && node.alert_level === "none") return "silent";
  return node.alert_level in LEVEL_LABELS ? node.alert_level : "silent";
}

function clockTime(ts) {
  return new Date(ts * 1000).toLocaleTimeString([], { hour12: false });
}

function trend(riseCmMin) {
  if (riseCmMin > 0) return `rising ${riseCmMin.toFixed(1)} cm/min`;
  if (riseCmMin < 0) return `falling ${Math.abs(riseCmMin).toFixed(1)} cm/min`;
  return "steady";
}

function element(tag, className, text) {
  const made = document.createElement(tag);
  if (className) made.className = className;
  if (text !== undefined) made.textContent = text;
  return made;
}

// Side panel

function nodeCard(node) {
  const level = shownLevel(node);
  const card = element("li", `node level--${level}`);

  const head = element("div", "node__head");
  const title = element("div");
  title.append(
    element("div", "node__name", node.name),
    element("div", "node__role", ROLE_LABELS[node.role] || node.role),
  );
  head.append(title, element("span", "node__level", LEVEL_LABELS[level]));
  card.append(head);

  if (node.level_cm !== null) {
    const reading = element("div", "node__reading", `${node.level_cm.toFixed(1)} cm `);
    reading.append(element("small", "", trend(node.rise_cm_min)));
    card.append(reading);
  }
  const heard = node.last_message_ts === null
    ? "Nothing received from this node yet"
    : `Last message at ${clockTime(node.last_message_ts)}`;
  card.append(element("div", "node__time", heard));
  return card;
}

function showSummary(nodes) {
  const heard = nodes.filter((node) => shownLevel(node) !== "silent");
  let level = "unknown";
  let text = "Waiting for data";
  if (heard.length > 0) {
    level = heard
      .map((node) => node.alert_level)
      .reduce((a, b) => (SEVERITY.indexOf(b) > SEVERITY.indexOf(a) ? b : a), "none");
    text = level === "none" ? "No alert" : `Highest alert level: ${LEVEL_LABELS[level]}`;
  }
  summary.className = `summary summary--${level}`;
  summary.textContent = text;
}

// Map

function pinIcon(role, level) {
  const levelClass = level ? ` level--${level}` : "";
  return L.divIcon({
    className: "",
    html: `<span class="pin pin--${role}${levelClass}"></span>`,
    iconSize: [26, 26],
    iconAnchor: [13, 13],
  });
}

function nodeLabel(node, level) {
  const label = element("span", "", `${node.name}: `);
  label.append(element("b", "", LEVEL_LABELS[level]));
  return label;
}

function createMap(data) {
  const mapElement = document.getElementById("map");
  if (typeof L === "undefined") {
    mapElement.classList.add("map__unavailable");
    mapElement.textContent =
      "The map library could not be loaded (no internet connection?). The node list is still live.";
    return null;
  }
  const created = L.map(mapElement);
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 19,
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
  }).addTo(created);

  const positions = data.nodes.map((node) => [node.lat, node.lon]);
  positions.push([data.gateway.lat, data.gateway.lon]);
  // Keep the whole site in view as the window changes size, until the officer
  // moves the map. (A page opened in a background tab starts with no real size.)
  let movedByHand = false;
  for (const type of ["pointerdown", "wheel", "keydown"]) {
    mapElement.addEventListener(type, () => { movedByHand = true; }, { passive: true });
  }
  const frameSite = () => {
    if (!movedByHand) created.fitBounds(positions, { padding: [40, 40], animate: false });
  };
  created.on("resize", frameSite);
  frameSite();

  // A schematic line from each wadi node to the one upstream of it.
  const byId = new Map(data.nodes.map((node) => [node.id, node]));
  for (const node of data.nodes) {
    const upstream = byId.get(node.upstream);
    if (upstream) {
      L.polyline([[upstream.lat, upstream.lon], [node.lat, node.lon]], {
        color: "#2f6f9f", weight: 3, opacity: 0.6, dashArray: "6 8",
      }).addTo(created);
    }
  }

  L.marker([data.gateway.lat, data.gateway.lon], {
    icon: pinIcon("gateway", null), keyboard: false,
  }).bindTooltip(data.gateway.name, { direction: "top", offset: [0, -8] }).addTo(created);
  return created;
}

function showOnMap(node) {
  const level = shownLevel(node);
  let marker = markers.get(node.id);
  if (marker && marker.level === level) return;
  const icon = pinIcon(node.role in ROLE_LABELS ? node.role : "wadi_node", level);
  if (marker) {
    marker.leaflet.setIcon(icon);
    marker.leaflet.setTooltipContent(nodeLabel(node, level));
  } else {
    marker = { leaflet: L.marker([node.lat, node.lon], { icon }).addTo(map) };
    marker.leaflet.bindTooltip(nodeLabel(node, level), {
      permanent: true, direction: "bottom", offset: [0, 14], className: "node-label",
    });
    markers.set(node.id, marker);
  }
  marker.level = level;
  // The most serious alert stays on top where markers overlap.
  marker.leaflet.setZIndexOffset(1000 * SEVERITY.indexOf(node.alert_level));
}

// Polling

function show(data) {
  siteName.textContent = data.site.name;
  placeholderNote.hidden = !data.site.positions_are_placeholders;
  showSummary(data.nodes);
  nodeList.replaceChildren(...data.nodes.map(nodeCard));
  if (!mapTried) {
    mapTried = true;
    map = createMap(data);
  }
  if (map !== null) data.nodes.forEach(showOnMap);
}

async function refresh() {
  try {
    const response = await fetch(NODES_URL, { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    show(await response.json());
    connection.className = "connection";
    connection.textContent = `Updated ${new Date().toLocaleTimeString([], { hour12: false })}`;
  } catch (error) {
    // Say so plainly: a frozen picture must not pass for a calm wadi.
    console.error(error);
    connection.className = "connection connection--lost";
    connection.textContent = "Backend not reachable";
  } finally {
    setTimeout(refresh, POLL_MS);
  }
}

refresh();
