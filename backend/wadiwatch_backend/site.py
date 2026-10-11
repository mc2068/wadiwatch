"""The site file: which nodes exist, what they are called and where they sit on the map,
which wadi each wadi node is mounted over, how often the site is expected to be heard
from, and what the all clear is judged against.

Node identifiers, wadi identifiers, intervals and the Watch thresholds must match the
firmware site configuration. Keys are not kept here.
"""

import json
from dataclasses import dataclass
from pathlib import Path

from .contract import GATEWAY, SIREN_NODE, WADI_NODE

BACKEND_DIR = Path(__file__).resolve().parent.parent
DEFAULT_SITE_FILE = BACKEND_DIR / "site" / "prototype.json"

# The firmware identifies a wadi with one byte, and zero is no wadi.
MAX_WADI_ID = 255


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
    # For a wadi node: the wadi it is mounted over. An all clear is issued for a
    # wadi and covers every wadi node on it.
    wadi: int | None = None
    # For a wadi node: the level at or above which it is at Watch, in centimetres
    # above the bed. The all clear is refused until the level has stayed below it.
    watch_level_cm: float | None = None


@dataclass(frozen=True)
class Wadi:
    id: int
    name: str


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
    wadis: tuple
    # How often every node sends a heartbeat. Node health is judged against it.
    heartbeat_interval_s: int
    # How often a wadi node measures in dry mode. A node that holds an alert
    # level reports each of those measurements; one that holds none sends only
    # its heartbeat.
    dry_mode_interval_s: int
    # How often a wadi node at Warning or above repeats its alert.
    alert_refresh_interval_s: int
    # How long the level must have stayed below the Watch threshold at every wadi
    # node of a wadi before an all clear for that wadi is accepted.
    all_clear_quiet_s: int
    # How long an all clear command is worth acting on. The gateway ignores an
    # older one (its `commandMaxAgeS`), and until then the backend treats the
    # command as on its way and does not issue a second one for the same wadi.
    all_clear_command_valid_s: int

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

    def wadi(self, wadi_id):
        """The wadi with this identifier, or None if the site has no such wadi."""
        for wadi in self.wadis:
            if wadi.id == wadi_id:
                return wadi
        return None

    def wadi_nodes_of(self, wadi_id):
        """The wadi nodes mounted over a wadi."""
        return tuple(node for node in self.wadi_nodes if node.wadi == wadi_id)

    def wadi_of(self, node_id):
        """The wadi a wadi node is mounted over, or None for any other unit."""
        for node in self.wadi_nodes:
            if node.id == node_id:
                return self.wadi(node.wadi)
        return None


INTERVALS = (
    "heartbeat_interval_s",
    "dry_mode_interval_s",
    "alert_refresh_interval_s",
    "all_clear_quiet_s",
    "all_clear_command_valid_s",
)


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
            wadi=entry.get("wadi"),
            watch_level_cm=entry.get("watch_level_cm"),
        )
        for entry in raw["nodes"]
    )
    if not isinstance(raw.get("wadis"), list):
        raise SiteError(f"{path}: wadis must list the wadis the wadi nodes are mounted over")
    wadis = tuple(Wadi(id=entry.get("id"), name=entry.get("name")) for entry in raw["wadis"])
    site = Site(
        name=raw["name"],
        positions_are_placeholders=bool(raw.get("positions_are_placeholders", False)),
        gateway=gateway,
        nodes=nodes,
        wadis=wadis,
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
    _check_wadis(site, path)


def _check_wadis(site, path):
    for wadi in site.wadis:
        if type(wadi.id) is not int or not 1 <= wadi.id <= MAX_WADI_ID:
            raise SiteError(f"{path}: a wadi id must be a whole number from 1 to {MAX_WADI_ID}")
        if type(wadi.name) is not str or not wadi.name:
            raise SiteError(f"{path}: wadi {wadi.id} needs a name")
    wadi_ids = [wadi.id for wadi in site.wadis]
    if len(set(wadi_ids)) != len(wadi_ids):
        raise SiteError(f"{path}: two wadis share an identifier")

    for node in site.nodes:
        if node.role != WADI_NODE:
            if node.wadi is not None or node.watch_level_cm is not None:
                raise SiteError(
                    f"{path}: node {node.id} is not a wadi node: it has no wadi and no watch_level_cm"
                )
            continue
        if node.wadi not in wadi_ids:
            raise SiteError(f"{path}: wadi of wadi node {node.id} must be one of the wadis listed")
        level = node.watch_level_cm
        if type(level) not in (int, float) or not level > 0:
            raise SiteError(
                f"{path}: watch_level_cm of wadi node {node.id} must be a number greater than 0"
            )
    for node in site.wadi_nodes:
        if node.upstream is None:
            continue
        upstream = next(other for other in site.wadi_nodes if other.id == node.upstream)
        if upstream.wadi != node.wadi:
            raise SiteError(
                f"{path}: wadi node {node.id} and its upstream node must be on the same wadi"
            )
    for wadi in site.wadis:
        if not site.wadi_nodes_of(wadi.id):
            raise SiteError(f"{path}: wadi {wadi.id} has no wadi node")
