"""The gateway serves the dashboard from a fixed whitelist of files."""

import pytest


def test_index_is_served_at_root(gateway):
    status, headers, body = gateway.request("GET", "/")
    assert status == 200
    assert headers["Content-Type"].startswith("text/html")
    assert b"Fleet Ops" in body


@pytest.mark.parametrize("path, content_type", [
    ("/dashboard/index.html", "text/html"),
    ("/dashboard/style.css", "text/css"),
    ("/dashboard/app.js", "text/javascript"),
    ("/dashboard/charts.js", "text/javascript"),
    ("/dashboard/fleet_map.js", "text/javascript"),
    ("/dashboard/city_map.json", "application/json"),
])
def test_dashboard_assets_are_served(gateway, path, content_type):
    status, headers, body = gateway.request("GET", path)
    assert status == 200
    assert headers["Content-Type"].startswith(content_type)
    assert headers["Cache-Control"] == "no-cache"
    assert body


@pytest.mark.parametrize("path", [
    "/dashboard/../CMakeLists.txt",
    "/dashboard/%2e%2e/CMakeLists.txt",
    "/dashboard/../../etc/passwd",
    "/dashboard/",
    "/dashboard/missing.js",
    "/style.css",
])
def test_paths_outside_the_whitelist_are_not_found(gateway, path):
    status, _, _ = gateway.request("GET", path)
    assert status == 404
