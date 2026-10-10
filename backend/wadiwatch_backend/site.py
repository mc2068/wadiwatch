"""The site file: which nodes exist, what they are called and where they sit on the map,
and how often the site is expected to be heard from.

Node identifiers and intervals must match the firmware site configuration.
Thresholds and keys are not kept here.
"""

import json
from dataclasses import dataclass
from pathlib import Path

from .contract import GATEWAY, SIREN_NODE, WADI_NODE

BACKEND_DIR = Path(__file__).resolve().parent.parent
DEFAULT_SITE_FILE = BACKEND_DIR / "site" / "prototype.json"


class SiteError(ValueError):
    """The site file is inconsistent."""


@dataclass(frozen=True)
class Node:
    id: int
    role: str
    name: str
    lat: float
    lon: float
    # For a wadi node: the wadi node directly upstream of it, if any.
    upstream: int | None = None


@dataclass(frozen=True)
class Gateway:
    id: int
    name: str
    lat: float
    lon: float


@dataclass(frozen=True)
class Site:
    name: str
    positions_are_placeholders: bool
    gateway: Gateway
    nodes: tuple
    # How often every node sends a heartbeat. Node health is judged against it.
    heartbeat_interval_s: int
    # How often a wadi node measures in dry mode: the longest gap between two
    # gateway messages while nothing is happening.
    dry_mode_interval_s: int
    # How often a wadi node at Warning or above repeats its alert.
    alert_refresh_interval_s: int

    @property
    def wadi_nodes(self):
        return tuple(node for node in self.nodes if node.role == WADI_NODE)

    @property
    def siren_nodes(self):
        return tuple(node for node in self.nodes if node.role == SIREN_NODE)

    def role_of(self, node_id):
        """The role of the unit with this identifier, or None if the site has no such unit."""
        if node_id == self.gateway.id:
            return GATEWAY
        for node in self.nodes:
            if node.id == node_id:
                return node.role
        return None


INTERVALS = ("heartbeat_interval_s", "dry_mode_interval_s", "alert_refresh_interval_s")


def _interval(raw, name, path):
    value = raw.get(name)
    if type(value) is not int or value <= 0:
        raise SiteError(f"{path}: {name} must be a whole number of seconds greater than 0")
    return value


def load_site(path):
    with open(path, encoding="utf-8") as site_file:
        raw = json.load(site_file)

    gateway = Gateway(
        id=raw["gateway"]["id"],
        name=raw["gateway"]["name"],
        lat=raw["gateway"]["lat"],
        lon=raw["gateway"]["lon"],
    )
    nodes = tuple(
        Node(
            id=entry["id"],
            role=entry["role"],
            name=entry["name"],
            lat=entry["lat"],
            lon=entry["lon"],
            upstream=entry.get("upstream"),
        )
        for entry in raw["nodes"]
    )
    site = Site(
        name=raw["name"],
        positions_are_placeholders=bool(raw.get("positions_are_placeholders", False)),
        gateway=gateway,
        nodes=nodes,
        **{name: _interval(raw, name, path) for name in INTERVALS},
    )
    _check(site, path)
    return site


def _check(site, path):
    identifiers = [site.gateway.id] + [node.id for node in site.nodes]
    if len(set(identifiers)) != len(identifiers):
        raise SiteError(f"{path}: two units share an identifier")
    wadi_node_ids = {node.id for node in site.wadi_nodes}
    for node in site.nodes:
        if node.role not in (WADI_NODE, SIREN_NODE):
            raise SiteError(f"{path}: node {node.id} has unknown role {node.role!r}")
        if node.upstream is not None and (
            node.role != WADI_NODE or node.upstream not in wadi_node_ids or node.upstream == node.id
        ):
            raise SiteError(f"{path}: upstream of node {node.id} must be another wadi node")
