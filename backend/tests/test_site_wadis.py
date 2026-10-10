"""The site file says which wadi each wadi node is mounted over and where its Watch threshold lies.

The all clear is issued for a wadi and judged against those thresholds, so a
site file that leaves them out, or gets them wrong, is refused when it is
loaded rather than when an officer needs the all clear.
"""

import json

import pytest

from wadiwatch_backend.site import DEFAULT_SITE_FILE, SiteError, load_site


def site_file_with(tmp_path, change):
    raw = json.loads(DEFAULT_SITE_FILE.read_text(encoding="utf-8"))
    change(raw)
    path = tmp_path / "site.json"
    path.write_text(json.dumps(raw), encoding="utf-8")
    return path


def wadi_node(raw, index=0):
    return [node for node in raw["nodes"] if node["role"] == "wadi_node"][index]


def siren_node(raw):
    return next(node for node in raw["nodes"] if node["role"] == "siren_node")


def test_the_prototype_site_has_one_wadi_with_both_wadi_nodes_on_it(site):
    (wadi,) = site.wadis

    assert wadi.name
    assert [node.wadi for node in site.wadi_nodes] == [wadi.id, wadi.id]
    assert site.wadi(wadi.id) == wadi
    assert site.wadi(wadi.id + 1) is None


def test_the_watch_thresholds_of_the_prototype_site_are_those_of_the_firmware_site_configuration(site):
    # firmware/lib/wadiwatch/src/wadiwatch/site_config.cpp: Watch at 30 % of a
    # bank-full depth of 200 cm, at both wadi nodes.
    assert [node.watch_level_cm for node in site.wadi_nodes] == [60.0, 60.0]


@pytest.mark.parametrize(
    "why, change",
    [
        ("no list of wadis", lambda raw: raw.pop("wadis")),
        ("a wadi node on no wadi", lambda raw: wadi_node(raw).pop("wadi")),
        ("a wadi node on a wadi that is not listed", lambda raw: wadi_node(raw).update(wadi=9)),
        ("a wadi with no wadi node", lambda raw: raw["wadis"].append({"id": 2, "name": "Other"})),
        ("two wadis with one identifier", lambda raw: raw["wadis"].append(dict(raw["wadis"][0]))),
        ("a wadi without a name", lambda raw: raw["wadis"][0].pop("name")),
        ("a wadi identifier of zero", lambda raw: raw["wadis"][0].update(id=0)),
        ("a wadi identifier the firmware cannot hold", lambda raw: raw["wadis"][0].update(id=256)),
        ("a wadi node without a Watch threshold", lambda raw: wadi_node(raw).pop("watch_level_cm")),
        ("a Watch threshold of zero", lambda raw: wadi_node(raw).update(watch_level_cm=0)),
        ("a Watch threshold in words", lambda raw: wadi_node(raw).update(watch_level_cm="60")),
        ("a Watch threshold that is true", lambda raw: wadi_node(raw).update(watch_level_cm=True)),
        ("a siren node on a wadi", lambda raw: siren_node(raw).update(wadi=1)),
        ("a siren node with a Watch threshold", lambda raw: siren_node(raw).update(watch_level_cm=60.0)),
    ],
)
def test_a_site_file_that_gets_its_wadis_wrong_is_refused(tmp_path, why, change):
    path = site_file_with(tmp_path, change)

    with pytest.raises(SiteError):
        load_site(path)


def test_a_wadi_node_and_its_upstream_node_must_be_on_the_same_wadi(tmp_path):
    def split(raw):
        raw["wadis"].append({"id": 2, "name": "Other"})
        wadi_node(raw, 1)["wadi"] = 2

    with pytest.raises(SiteError, match="same wadi"):
        load_site(site_file_with(tmp_path, split))


def test_two_wadis_each_with_its_own_wadi_node_load(tmp_path):
    def split(raw):
        raw["wadis"].append({"id": 2, "name": "Other"})
        wadi_node(raw, 1).update(wadi=2, upstream=None)

    site = load_site(site_file_with(tmp_path, split))

    assert [len(site.wadi_nodes_of(wadi.id)) for wadi in site.wadis] == [1, 1]
