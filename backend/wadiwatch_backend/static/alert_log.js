// WadiWatch dashboard: the alert log, officer sign-in and acknowledgement.
//
// Self-contained on purpose: it shares nothing with dashboard.js but the page
// and the colours in dashboard.css.
"use strict";

(() => {
  const ALERT_LOG_URL = "api/alert-log";
  const SESSION_URL = "api/session";
  const CSRF_HEADER = "X-CSRF-Token";
  const POLL_MS = 2000;

  const LEVEL_LABELS = { watch: "Watch", warning: "Warning", evacuate: "Evacuate" };
  const TYPE_LABELS = { alert: "Alert", test: "Test", all_clear: "All clear" };
  // Siren nodes sound at these levels, so a missing acknowledgement matters.
  const SIREN_LEVELS = ["warning", "evacuate"];

  const byId = (id) => document.getElementById(id);
  const officerName = byId("officer-name");
  const signInOpen = byId("sign-in-open");
  const signOutButton = byId("sign-out");
  const requests = byId("acknowledgement-requests");
  const logEntries = byId("alert-log-entries");
  const logStatus = byId("alert-log-status");
  const dialog = byId("sign-in");
  const form = byId("sign-in-form");
  const signInNote = byId("sign-in-note");
  const signInError = byId("sign-in-error");
  const signInSubmit = byId("sign-in-submit");
  const pageTitle = document.title;

  // The session cookie is HttpOnly, so the script never sees it. It holds only
  // the officer's account name and the CSRF token, in memory, for this page.
  let officer = null;
  let csrfToken = null;
  let entries = null;
  let shown = "";

  function make(tag, className, text) {
    const made = document.createElement(tag);
    if (className) made.className = className;
    if (text !== undefined) made.textContent = text;
    return made;
  }

  function timeOf(ts) {
    const date = new Date(ts * 1000);
    const clock = date.toLocaleTimeString([], { hour12: false });
    const today = date.toDateString() === new Date().toDateString();
    const made = make("time", "", today ? clock : `${date.toLocaleDateString()} ${clock}`);
    made.dateTime = date.toISOString();
    return made;
  }

  function sentence(...parts) {
    const made = make("span");
    made.append(...parts);
    return made;
  }

  // Officer sign-in

  function showOfficer() {
    officerName.hidden = officer === null;
    officerName.textContent = officer === null ? "" : `Signed in as ${officer}`;
    signInOpen.hidden = officer !== null;
    signOutButton.hidden = officer === null;
  }

  async function loadSession() {
    const response = await fetch(SESSION_URL, { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const session = await response.json();
    officer = session.signed_in ? session.officer : null;
    csrfToken = session.signed_in ? session.csrf_token : null;
  }

  function openSignIn(note) {
    form.reset();
    signInNote.textContent = note || "Sign in to acknowledge alerts.";
    signInError.hidden = true;
    if (!dialog.open) dialog.showModal();
    byId("sign-in-name").focus();
  }

  async function signIn(event) {
    event.preventDefault();
    signInSubmit.disabled = true;
    signInError.hidden = true;
    try {
      const response = await fetch(SESSION_URL, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          name: byId("sign-in-name").value.trim(),
          password: byId("sign-in-password").value,
        }),
      });
      if (response.status === 401) {
        signInError.textContent = "Wrong name or password.";
        signInError.hidden = false;
        byId("sign-in-password").value = "";
        byId("sign-in-password").focus();
        return;
      }
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const session = await response.json();
      officer = session.officer;
      csrfToken = session.csrf_token;
      form.reset();
      dialog.close();
      await refreshLog();
    } catch (error) {
      console.error(error);
      signInError.textContent = "Sign-in failed: the backend could not be reached.";
      signInError.hidden = false;
    } finally {
      signInSubmit.disabled = false;
    }
  }

  async function signOut() {
    signOutButton.disabled = true;
    try {
      const response = await fetch(SESSION_URL, {
        method: "DELETE",
        headers: { [CSRF_HEADER]: csrfToken || "" },
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      officer = null;
      csrfToken = null;
    } catch (error) {
      console.error(error);
      logStatus.textContent = "Sign-out failed. Try again.";
    } finally {
      signOutButton.disabled = false;
    }
    await refreshLog();
  }

  // Acknowledgement

  async function acknowledge(entry, button) {
    button.disabled = true;
    let problem = "";
    try {
      const response = await fetch(`api/alerts/${entry.id}/acknowledgement`, {
        method: "POST",
        headers: { [CSRF_HEADER]: csrfToken || "" },
      });
      if (response.status === 401) {
        officer = null;
        csrfToken = null;
        openSignIn("Your session has ended. Sign in again to acknowledge the alert.");
      } else if (response.status === 403) {
        // The token this page holds is out of date (signed in again in another tab).
        await loadSession();
        problem = "The acknowledgement was not recorded. Press Acknowledge again.";
      } else if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
    } catch (error) {
      console.error(error);
      problem = "The acknowledgement was not recorded: the backend could not be reached.";
    }
    logStatus.textContent = problem;
    button.disabled = false;
    await refreshLog();
  }

  function acknowledgeControl(entry, className) {
    if (officer === null) {
      const button = make("button", className, "Sign in to acknowledge");
      button.type = "button";
      button.addEventListener("click", () => openSignIn());
      return button;
    }
    const button = make("button", className, "Acknowledge");
    button.type = "button";
    button.addEventListener("click", () => acknowledge(entry, button));
    return button;
  }

  // The request that stays on screen at Evacuate

  // The request sticks to the top of the window. A link into the page (a node
  // flag jumping to the node health section) must land below it, not under it.
  function keepLinksBelowRequests() {
    document.documentElement.style.scrollPaddingTop = `${requests.offsetHeight}px`;
  }

  function showRequests(waiting) {
    requests.hidden = waiting.length === 0;
    requests.replaceChildren(...waiting.map((entry) => {
      const request = make("div", "ack-request");
      const text = sentence(
        make("b", "", `${LEVEL_LABELS[entry.alert_level]} at ${entry.node_name}`),
        " since ",
        timeOf(entry.ts),
        ". No officer has acknowledged this alert.",
      );
      text.className = "ack-request__text";
      request.append(text, acknowledgeControl(entry, "ack-request__button"));
      return request;
    }));
    document.title = waiting.length === 0
      ? pageTitle
      : `Acknowledge ${LEVEL_LABELS[waiting[0].alert_level]}: ${pageTitle}`;
    keepLinksBelowRequests();
  }

  // Alert log

  function entryCell(entry) {
    const cell = make("td", "alert-log__entry");
    cell.append(make("span", "alert-log__type", TYPE_LABELS[entry.type] || entry.type));
    if (entry.type === "alert") {
      const label = LEVEL_LABELS[entry.alert_level] || entry.alert_level;
      cell.append(make("span", `alert-log__level level--${entry.alert_level}`, label));
      if (entry.in_force) cell.append(make("span", "alert-log__in-force", "in force"));
    }
    return cell;
  }

  function nodeCell(entry) {
    if (entry.type === "all_clear") return make("td", "", entry.wadi_node_name);
    if (entry.type === "test") return make("td", "", `${entry.node_name}, local test`);
    return make("td", "", entry.node_name);
  }

  function sirenCell(entry) {
    const cell = make("td");
    if (entry.type !== "alert") return cell;
    const list = make("ul", "alert-log__sirens");
    for (const siren of entry.siren_nodes) {
      if (siren.acknowledged_ts !== null) {
        const item = make("li", "alert-log__siren alert-log__siren--yes");
        item.append(`${siren.name}: acknowledged `, timeOf(siren.acknowledged_ts));
        list.append(item);
      } else {
        const expected = SIREN_LEVELS.includes(entry.alert_level);
        list.append(make(
          "li",
          `alert-log__siren ${expected ? "alert-log__siren--no" : "alert-log__siren--quiet"}`,
          `${siren.name}: no acknowledgement`,
        ));
      }
    }
    cell.append(list);
    return cell;
  }

  function officerCell(entry) {
    const cell = make("td", "alert-log__officer");
    if (entry.type !== "alert") return cell;
    if (entry.acknowledgement !== null) {
      cell.append("Acknowledged ", timeOf(entry.acknowledgement.ts));
      // The name is sent to signed-in officers only.
      if (entry.acknowledgement.officer) cell.append(` by ${entry.acknowledgement.officer}`);
    } else if (entry.in_force && officer !== null) {
      cell.append(acknowledgeControl(entry, "alert-log__button"));
    } else {
      const needed = entry.acknowledgement_requested;
      cell.append(make(
        "span",
        needed ? "alert-log__needed" : "alert-log__quiet",
        needed ? "Acknowledgement needed" : "Not acknowledged",
      ));
    }
    return cell;
  }

  function showLog() {
    if (entries.length === 0) {
      const cell = make("td", "alert-log__empty", "No alert, test or all clear has been recorded yet.");
      cell.colSpan = 5;
      const row = make("tr");
      row.append(cell);
      logEntries.replaceChildren(row);
      return;
    }
    logEntries.replaceChildren(...entries.map((entry) => {
      const row = make("tr", `alert-log__row alert-log__row--${entry.type}`);
      const time = make("td", "alert-log__time");
      time.append(timeOf(entry.ts));
      row.append(time, entryCell(entry), nodeCell(entry), sirenCell(entry), officerCell(entry));
      return row;
    }));
  }

  function show() {
    showOfficer();
    if (entries === null) return;
    // Redraw only on a change, so a button keeps the focus between polls.
    const signature = JSON.stringify([officer, entries]);
    if (signature === shown) return;
    shown = signature;
    showRequests(entries.filter((entry) => entry.acknowledgement_requested));
    showLog();
  }

  async function refreshLog() {
    try {
      const response = await fetch(ALERT_LOG_URL, { cache: "no-store" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const log = await response.json();
      // The session ended, or began in another tab: learn who we are again.
      if (log.signed_in_as !== officer) await loadSession();
      entries = log.entries;
      logStatus.classList.remove("alert-log__status--lost");
      if (logStatus.dataset.lost) {
        delete logStatus.dataset.lost;
        logStatus.textContent = "";
      }
    } catch (error) {
      console.error(error);
      logStatus.classList.add("alert-log__status--lost");
      logStatus.dataset.lost = "yes";
      logStatus.textContent = "Backend not reachable: the alert log may be out of date.";
    }
    show();
  }

  async function poll() {
    await refreshLog();
    setTimeout(poll, POLL_MS);
  }

  // Its text wraps differently, so its height changes, when the window does.
  window.addEventListener("resize", keepLinksBelowRequests);
  signInOpen.addEventListener("click", () => openSignIn());
  signOutButton.addEventListener("click", signOut);
  form.addEventListener("submit", signIn);
  byId("sign-in-cancel").addEventListener("click", () => {
    form.reset();
    dialog.close();
  });

  loadSession().catch((error) => console.error(error)).finally(poll);
})();
