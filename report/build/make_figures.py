"""Generate the report figures into report/figures/. Run: python make_figures.py"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle

OUT = Path(__file__).resolve().parents[1] / "figures"
OUT.mkdir(parents=True, exist_ok=True)

NAVY, TEAL, AMBER, RED, GREY, LIGHT, INK = (
    "#1F3A5F", "#1B9AAA", "#F2A541", "#D1495B", "#6B7280", "#F3F6FA", "#1F2933")
plt.rcParams.update({"font.family": "Calibri", "font.size": 11, "text.color": INK})


def canvas(w, h):
    fig, ax = plt.subplots(figsize=(w, h), dpi=200)
    ax.set_xlim(0, w)
    ax.set_ylim(0, h)
    ax.axis("off")
    return fig, ax


def box(ax, x, y, w, h, title, lines=(), fc=LIGHT, ec=NAVY, tc=NAVY, title_size=13, body_size=11):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.08",
                                fc=fc, ec=ec, lw=1.4))
    top = y + h - 0.22
    ax.text(x + w / 2, top, title, ha="center", va="top", fontsize=title_size, fontweight="bold", color=tc)
    for i, line in enumerate(lines):
        ax.text(x + w / 2, top - 0.34 - i * 0.27, line, ha="center", va="top", fontsize=body_size, color=INK)


def arrow(ax, p, q, color=NAVY, style="-", lw=1.6, label=None, label_offset=(0, 0.12), rad=0.0):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle="-|>", mutation_scale=13, lw=lw, color=color,
                                 linestyle=style, connectionstyle=f"arc3,rad={rad}",
                                 shrinkA=2, shrinkB=2))
    if label:
        mx, my = (p[0] + q[0]) / 2 + label_offset[0], (p[1] + q[1]) / 2 + label_offset[1]
        ax.text(mx, my, label, ha="center", va="bottom", fontsize=10.5, color=color, fontstyle="italic")


def save(fig, name):
    fig.savefig(OUT / name, bbox_inches="tight", pad_inches=0.08, facecolor="white")
    plt.close(fig)


def fig_flow():
    fig, ax = canvas(10, 2.6)
    stages = [
        ("1. DETECT", TEAL, ["Wadi node measures", "water level and rain", "Decides alert level", "on the node itself"]),
        ("2. COMMUNICATE", NAVY, ["LoRa direct to sirens", "LoRa to gateway", "SMS over GSM", "Works without internet"]),
        ("3. COORDINATE", AMBER, ["Dashboard for the", "municipality and", "Civil Protection", "Acknowledge and assign"]),
        ("4. RESPOND", RED, ["Households move up", "Farmers move stock", "Roads are closed", "Teams are dispatched"]),
    ]
    w, gap, x0 = 2.1, 0.4, 0.2
    for i, (title, color, lines) in enumerate(stages):
        x = x0 + i * (w + gap)
        box(ax, x, 0.75, w, 1.6, title, lines, fc="white", ec=color, tc=color)
        if i < len(stages) - 1:
            arrow(ax, (x + w + 0.04, 1.55), (x + w + gap - 0.04, 1.55), color=GREY, lw=2)
    ax.text(5, 0.38, "Target: siren within 10 seconds of detection; SMS within 60 seconds (to be measured on the prototype)",
            ha="center", va="center", fontsize=11, color=GREY)
    save(fig, "fig1_flow.png")


def fig_architecture():
    fig, ax = canvas(10, 5.6)
    ax.add_patch(Rectangle((0.1, 0.15), 2.75, 5.3, fc="#EAF6F8", ec="none"))
    ax.add_patch(Rectangle((3.05, 0.15), 3.2, 5.3, fc="#F3F6FA", ec="none"))
    ax.add_patch(Rectangle((6.45, 0.15), 3.45, 5.3, fc="#FDF3E3", ec="none"))
    for x, label in ((1.47, "IN THE WADI"), (4.65, "IN THE TOWN"), (8.17, "PEOPLE WHO ACT")):
        ax.text(x, 5.22, label, ha="center", va="center", fontsize=12, fontweight="bold", color=GREY)

    box(ax, 0.3, 3.55, 2.35, 1.25, "Wadi node (upstream)", ["Level + rain, solar", "Edge alert decision"], fc="white", ec=TEAL, tc=TEAL)
    box(ax, 0.3, 1.85, 2.35, 1.25, "Wadi node (mid-reach)", ["Confirms the flood wave", "Rate of rise"], fc="white", ec=TEAL, tc=TEAL)
    box(ax, 0.3, 0.4, 2.35, 1.0, "Wadi node (in town)", ["Level at the bridge"], fc="white", ec=TEAL, tc=TEAL)

    box(ax, 3.35, 3.55, 2.6, 1.25, "Siren nodes", ["Siren + strobe, solar", "Always listening on LoRa"], fc="white", ec=RED, tc=RED)
    box(ax, 3.35, 1.55, 2.6, 1.45, "Gateway", ["Municipal building, battery", "LoRa + GSM modem", "Stores and forwards"], fc="white", ec=NAVY, tc=NAVY)
    box(ax, 3.35, 0.33, 2.6, 0.9, "Cloud + dashboard", ["When internet is up"], fc="white", ec=GREY, tc=GREY)

    box(ax, 6.75, 3.55, 2.85, 1.25, "Households near the wadi", ["Hear the siren, see the strobe", "No phone needed"], fc="white", ec=RED, tc=RED)
    box(ax, 6.75, 1.95, 2.85, 1.1, "Registered recipients", ["Residents, farmers, schools", "SMS in Arabic / French"], fc="white", ec=NAVY, tc=NAVY)
    box(ax, 6.75, 0.4, 2.85, 1.1, "Municipality, Civil Protection", ["Map, alert log, node health", "Acknowledge and dispatch"], fc="white", ec=AMBER, tc="#B36B00", title_size=11.5)

    # Life-safety path (red, solid)
    arrow(ax, (2.65, 4.3), (3.35, 4.3), color=RED, lw=2.4)
    arrow(ax, (2.65, 2.75), (3.35, 3.85), color=RED, lw=2.4)
    arrow(ax, (5.95, 4.2), (6.75, 4.2), color=RED, lw=2.4)
    # Information path (navy, dashed)
    arrow(ax, (2.65, 3.75), (3.35, 2.75), color=NAVY, style="--")
    arrow(ax, (2.65, 2.3), (3.35, 2.3), color=NAVY, style="--")
    arrow(ax, (2.65, 0.95), (3.35, 1.85), color=NAVY, style="--")
    arrow(ax, (5.95, 2.45), (6.75, 2.5), color=NAVY, style="--")
    arrow(ax, (4.65, 1.55), (4.65, 1.2), color=GREY, style="--")
    arrow(ax, (5.95, 0.8), (6.75, 0.9), color=GREY, style="--")

    ax.plot([0.35, 0.95], [-0.05, -0.05], color=RED, lw=2.4, clip_on=False)
    ax.text(1.05, -0.05, "Life-safety path: LoRa direct, no gateway, internet, GSM or grid needed", va="center", fontsize=11, color=RED, clip_on=False)
    ax.plot([0.35, 0.95], [-0.32, -0.32], color=NAVY, lw=1.6, ls="--", clip_on=False)
    ax.text(1.05, -0.32, "Information path: LoRa to gateway, then SMS over GSM and dashboard over internet", va="center", fontsize=11, color=NAVY, clip_on=False)
    save(fig, "fig2_architecture.png")


def fig_node():
    fig, ax = canvas(10, 4.3)
    box(ax, 0.2, 3.0, 1.9, 1.05, "Solar panel", ["6 V, 5 W"], fc="white", ec=AMBER, tc="#B36B00")
    box(ax, 2.55, 3.0, 2.0, 1.05, "Charge controller", ["Battery protection"], fc="white", ec=AMBER, tc="#B36B00")
    box(ax, 5.0, 3.0, 1.9, 1.05, "Li-ion battery", ["3.7 V, 3000 mAh"], fc="white", ec=AMBER, tc="#B36B00")
    box(ax, 7.35, 3.0, 2.4, 1.05, "3.3 V regulator", ["Low quiescent current"], fc="white", ec=AMBER, tc="#B36B00")
    for a, b in (((2.1, 3.52), (2.55, 3.52)), ((4.55, 3.52), (5.0, 3.52)), ((6.9, 3.52), (7.35, 3.52))):
        arrow(ax, a, b, color=AMBER, lw=2)

    box(ax, 3.6, 0.95, 2.8, 1.5, "ESP32 microcontroller", ["Deep sleep between samples", "Median filter, rate of rise", "Alert level + message signing"], fc="#EAF6F8", ec=NAVY, tc=NAVY)
    arrow(ax, (7.6, 3.0), (6.2, 2.47), color=AMBER, lw=2)

    box(ax, 0.2, 1.7, 2.6, 0.95, "Ultrasonic level sensor", ["Waterproof, 0.2 to 6 m"], fc="white", ec=TEAL, tc=TEAL, title_size=12)
    box(ax, 0.2, 0.45, 2.6, 0.95, "Tipping-bucket rain gauge", ["Wakes the node on rain"], fc="white", ec=TEAL, tc=TEAL, title_size=12)
    arrow(ax, (2.8, 2.15), (3.6, 1.95), color=TEAL)
    arrow(ax, (2.8, 0.95), (3.6, 1.35), color=TEAL)

    box(ax, 7.2, 1.7, 2.55, 0.95, "LoRa radio, 868 MHz", ["To siren nodes and gateway"], fc="white", ec=RED, tc=RED, title_size=12)
    box(ax, 7.2, 0.45, 2.55, 0.95, "Tamper + battery monitor", ["Reported in the heartbeat"], fc="white", ec=GREY, tc=GREY, title_size=12)
    arrow(ax, (6.4, 1.95), (7.2, 2.15), color=RED)
    arrow(ax, (7.2, 0.95), (6.4, 1.35), color=GREY)
    ax.text(5, 0.12, "All parts are off-the-shelf modules in an IP65 enclosure, mounted above flood level on a bridge or culvert",
            ha="center", va="center", fontsize=11, color=GREY)
    save(fig, "fig3_node.png")


def fig_gantt():
    tasks = [
        ("Concept, report and video (Phase 1)", 0, 2, GREY),
        ("Sprint 1: wadi node firmware, sensor bench tests, LoRa link", 2, 2, TEAL),
        ("Sprint 2: siren node, gateway, SMS alerts", 4, 2, TEAL),
        ("Sprint 3: dashboard, integration, enclosure, power tests", 6, 2, TEAL),
        ("Sprint 4: field test, range test, repository, demo", 8, 2, TEAL),
        ("Final presentation and prototype (10 Dec 2026)", 10, 0.6, RED),
        ("Site survey and threshold calibration with the municipality", 12, 5, NAVY),
        ("Pilot installation on one wadi, before the autumn rains", 17, 4, NAVY),
        ("Pilot operation and evaluation through a rain season", 21, 5, NAVY),
        ("Scale-up across Cap Bon", 26, 6, AMBER),
    ]
    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=200)
    for i, (name, start, dur, color) in enumerate(tasks):
        y = len(tasks) - 1 - i
        ax.barh(y, dur, left=start, height=0.56, color=color, edgecolor="none")
        ax.text(-0.4, y, name, ha="right", va="center", fontsize=11)
    ax.axvline(11.2, color=GREY, lw=1, ls=":")
    ax.set_xlim(0, 32)
    ax.set_ylim(-0.7, len(tasks) - 0.3)
    ax.set_yticks([])
    ax.set_xticks([0, 2, 4, 6, 8, 10, 12, 17, 21, 26, 32])
    ax.set_xticklabels(["Oct\n2026", "mid\nOct", "end\nOct", "mid\nNov", "end\nNov", "10\nDec", "Q1\n2027", "Q2", "Q3", "Q4", "2028"], fontsize=10)
    ax.text(5.5, len(tasks) - 0.25, "Challenge period", ha="center", fontsize=10.5, color=GREY)
    ax.text(21.5, len(tasks) - 0.25, "After the challenge (not to scale)", ha="center", fontsize=10.5, color=GREY)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.tick_params(axis="x", length=3, color=GREY)
    save(fig, "fig4_gantt.png")


def fig_risk():
    # (id, likelihood 1-5, impact 1-5)
    risks = [("R1", 3, 4), ("R2", 3, 5), ("R3", 3, 3), ("R4", 2, 4), ("R5", 3, 3),
             ("R6", 2, 3), ("R7", 3, 4), ("R8", 3, 2), ("R9", 3, 3), ("R10", 1, 5)]
    fig, ax = plt.subplots(figsize=(6.2, 4.6), dpi=200)
    for lik in range(1, 6):
        for imp in range(1, 6):
            score = lik * imp
            color = "#DDEFD9" if score <= 4 else "#FBE9C6" if score <= 9 else "#F6CBD0"
            ax.add_patch(Rectangle((imp - 0.5, lik - 0.5), 1, 1, fc=color, ec="white", lw=2))
    cells = {}
    for rid, lik, imp in risks:
        cells.setdefault((imp, lik), []).append(rid)
    for (imp, lik), ids in cells.items():
        ax.text(imp, lik, "\n".join(", ".join(ids[i:i + 2]) for i in range(0, len(ids), 2)),
                ha="center", va="center", fontsize=12, fontweight="bold", color=NAVY)
    ax.set_xlim(0.5, 5.5)
    ax.set_ylim(0.5, 5.5)
    ax.set_xticks(range(1, 6))
    ax.set_yticks(range(1, 6))
    ax.set_xticklabels(["1\nMinor", "2", "3\nModerate", "4", "5\nLife-threatening"], fontsize=9)
    ax.set_yticklabels(["1 Rare", "2", "3 Possible", "4", "5 Likely"], fontsize=9)
    ax.set_xlabel("Impact", fontsize=10.5, fontweight="bold", color=GREY)
    ax.set_ylabel("Likelihood", fontsize=10.5, fontweight="bold", color=GREY)
    for side in ax.spines.values():
        side.set_visible(False)
    ax.tick_params(length=0)
    save(fig, "fig5_risk.png")


if __name__ == "__main__":
    fig_flow()
    fig_architecture()
    fig_node()
    fig_gantt()
    fig_risk()
    print("figures written to", OUT)
