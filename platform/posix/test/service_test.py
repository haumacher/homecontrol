#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# The controller executable as the Linux package runs it (homecontrol #95, doc/linux.md), against
# the built binary: the port and bind address from the unit's environment, the data beside the
# settings file in a data directory, the language packs from the UI files and none fetched, no update
# channel, restart and factory reset over the API ending the process (systemd starts it again),
# and an image with Zigbee bindings next to KNX datapoints loading without a Zigbee adapter.
#
#   service_test.py <homecontrol executable> <spec dir> <web UI dir> <case>
#
# One ctest entry per case (platform/posix/CMakeLists.txt). Standard library only.

import http.cookiejar
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

PROGRAM = sys.argv[1]
SPEC = sys.argv[2]
WWW = sys.argv[3]  # the checkout's UI files: the build's default may be the package's, not installed yet
CASE = sys.argv[4]


def free_port():
    """A port whose next port is free as well (the WebSocket)."""
    for _ in range(50):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        if port >= 65534:
            continue
        try:
            with socket.socket() as a, socket.socket() as b:
                a.bind(("127.0.0.1", port))
                b.bind(("127.0.0.1", port + 1))
            return port
        except OSError:
            continue
    raise RuntimeError("no free port pair")


class Controller:
    def __init__(self, data, port, args=(), env=None):
        self.data = data
        self.port = port
        self.base = f"http://127.0.0.1:{port}"
        environment = dict(os.environ)
        environment.pop("HOMECONTROL_HTTP_PORT", None)
        environment.pop("HOMECONTROL_HTTP_BIND", None)
        environment.update(env or {"HOMECONTROL_HTTP_PORT": str(port), "HOMECONTROL_HTTP_BIND": "127.0.0.1"})
        self.log_path = os.path.join(data, "log.txt")
        self.log = open(self.log_path, "ab")
        # As the unit: the settings file in the data directory, the working directory there too.
        self.process = subprocess.Popen(
            [PROGRAM, "--settings", os.path.join(data, "homecontrol.settings"), "--www", WWW, *args],
            cwd=data, env=environment, stdout=self.log, stderr=subprocess.STDOUT)
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))
        deadline = time.time() + 10
        while True:
            try:
                self.request("GET", "/api/auth/session")
                return
            except (urllib.error.URLError, ConnectionError):
                if self.process.poll() is not None or time.time() > deadline:
                    raise AssertionError("the controller did not come up:\n" + self.text())
                time.sleep(0.1)

    def text(self):
        if not self.log.closed:
            self.log.flush()
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            return f.read()

    def request(self, method, path, body=None, raw=False):
        data = None
        headers = {"X-Requested-With": "HomeControl"}
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            headers["Content-Type"] = "application/octet-stream" if isinstance(body, bytes) else "application/json"
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=headers)
        try:
            with self.opener.open(req, timeout=10) as r:
                payload = r.read()
                status = r.status
        except urllib.error.HTTPError as e:
            payload = e.read()
            status = e.code
        if raw:
            return status, payload
        return status, (json.loads(payload) if payload else None)

    def wait_exit(self, timeout=10):
        try:
            return self.process.wait(timeout)
        finally:
            self.log.close()

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(10)
        self.log.close()


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def version():
    out = subprocess.run([PROGRAM, "--version"], capture_output=True, text=True, check=True).stdout
    return out.split()[1]


def case_environment(data):
    port = free_port()
    c = Controller(data, port)
    try:
        check(f"web UI on 127.0.0.1 port {port}" in c.text(), "the bind address and port from the environment:\n" + c.text())
        check(f"WebSocket on port {port + 1}" in c.text(), "the WebSocket on the next port")
        # Bound to 127.0.0.1 only: the WebSocket port answers there.
        with socket.create_connection(("127.0.0.1", port + 1), timeout=3):
            pass
        status, session = c.request("GET", "/api/auth/session")
        check(status == 200 and session["onboarded"] is False, f"onboarding first: {status} {session}")
        # The controller's files live in the data directory, beside the settings file.
        check(onboard(c) is None and os.path.exists(os.path.join(data, "homecontrol.settings")),
              f"the settings in the data directory: {os.listdir(data)}")
    finally:
        c.stop()
    # The command line wins over the environment; the commented-out defaults (empty) are unset.
    other = free_port()
    c = Controller(data, other, args=("--http", str(other), "--bind", "127.0.0.1"),
                   env={"HOMECONTROL_HTTP_PORT": "", "HOMECONTROL_HTTP_BIND": ""})
    c.stop()
    # A bad value in /etc/default/homecontrol: the process ends at once, with the reason.
    bad = subprocess.run([PROGRAM, "--settings", os.path.join(data, "homecontrol.settings")], cwd=data,
                         env={**os.environ, "HOMECONTROL_HTTP_PORT": "eighty"}, capture_output=True, text=True,
                         timeout=10)
    check(bad.returncode == 2, f"refused: {bad.returncode}")
    check("HOMECONTROL_HTTP_PORT=eighty: the port is 0..65534" in bad.stderr, bad.stderr)


def onboard(c):
    status, body = c.request("POST", "/api/auth/onboard", {"password": "correct horse"})
    check(status == 200, f"onboard: {status} {body}")


def case_linux(data):
    port = free_port()
    c = Controller(data, port)
    try:
        onboard(c)
        status, s = c.request("GET", "/api/status")
        check(status == 200, f"status {status}")
        check(s["device"]["version"] == version(), f"the version of --version: {s['device']['version']}")
        update = s["update"]
        check(update["available"] is False, f"no self-update: {update}")
        check("source" not in update, f"no update source: {update}")
        status, settings = c.request("GET", "/api/settings")
        keys = [e["key"] for e in settings["settings"]]
        check("update.channel" not in keys, f"no update channel: {keys}")
        check("ui.lang" in keys and "knx.gateway" in keys, f"the other settings: {keys}")
        check("i18n" not in settings, "no language pack download")
        status, body = c.request("PUT", "/api/settings/update.channel", {"value": "beta"})
        check(status == 403 and body["code"] == "settings.not_editable", f"update.channel refused: {status} {body}")
        status, body = c.request("POST", "/api/system/update/check")
        check(status == 501, f"no update check: {status} {body}")
        status, body = c.request("POST", "/api/lang/sync")
        check(status == 501, f"no language pack sync: {status} {body}")
        # The language packs come with the UI files.
        status, body = c.request("PUT", "/api/settings/ui.lang", {"value": "de"})
        check(status == 200, f"ui.lang: {status} {body}")
        status, pack = c.request("GET", "/lang/de.json")
        check(status == 200 and pack.get("lang.name") == "Deutsch", f"/lang/de.json: {status}")
        status, raw = c.request("GET", "/i18n/de.json", raw=True)
        check(status == 204, f"/i18n/de.json without a download: {status}")
        status, raw = c.request("GET", "/", raw=True)
        check(status == 200 and b"<html" in raw.lower(), "the UI")
        time.sleep(0.5)
        check("i18n" not in c.text() and "manifest" not in c.text(), "nothing fetched:\n" + c.text())
    finally:
        c.stop()


def case_restart(data):
    port = free_port()
    c = Controller(data, port)
    onboard(c)
    check(c.request("PUT", "/api/settings/tz", {"value": "EST5EDT,M3.2.0,M11.1.0"})[0] == 200, "tz")
    status, body = c.request("POST", "/api/system/restart")
    check(status == 200 and body["action"] == "restart", f"restart: {status} {body}")
    code = c.wait_exit()
    check(code == 0, f"the process ends for the supervisor: {code}")
    check("stopping on a restart over the API" in c.text(), c.text())
    # The supervisor starts it again: the settings stayed.
    c = Controller(data, port)
    try:
        status, session = c.request("GET", "/api/auth/session")
        check(session["onboarded"] is True, f"still onboarded: {session}")
        check(c.request("POST", "/api/auth/login", {"password": "correct horse"})[0] == 200, "login")
        status, s = c.request("GET", "/api/status")
        check(s["time"]["tz"] == "EST5EDT,M3.2.0,M11.1.0", f"settings kept: {s['time']}")
        status, s = c.request("GET", "/api/status")
        name = s["device"]["name"]
        status, body = c.request("POST", "/api/system/factory-reset", {"confirm": name})
        check(status == 200, f"factory reset: {status} {body}")
        code = c.wait_exit()
        check(code == 0, f"the process ends after the reset: {code}")
    finally:
        c.stop()
    c = Controller(data, port)
    try:
        status, session = c.request("GET", "/api/auth/session")
        check(session["onboarded"] is False, f"onboarding again: {session}")
    finally:
        c.stop()


def case_zigbee(data):
    # The Zigbee bindings of the corpus next to a KNX datapoint, compiled for the controller.
    image = os.path.join(SPEC, "fixtures", "images", "zigbee-bindings.bin")
    port = free_port()
    c = Controller(data, port, args=("--image", image))
    try:
        onboard(c)
        text = c.text()
        check("image 'zigbee-bindings' revision 1" in text and "4 Zigbee binding(s)" in text, text)
        check("4 Zigbee-bound datapoint(s) without a Zigbee adapter on this platform" in text, text)
        status, dp = c.request("GET", "/api/datapoints/bath.temperature")
        check(status == 200, f"the bound datapoint is in the model: {status}")
        check(dp.get("value") is None, f"without a value (no Zigbee here): {dp}")
        status, body = c.request("POST", "/api/datapoints/living.ceiling/command", {"value": True})
        check(status == 409 and body["code"] == "dp.refused", f"a command to a Zigbee datapoint is refused: {status} {body}")
        check("command to 'living.ceiling' refused: it is bound to Zigbee" in c.text(), "the reason in the log")
        status, body = c.request("GET", "/api/zigbee")
        check(status == 501, f"the Zigbee API is not available: {status} {body}")
    finally:
        c.stop()


CASES = {"environment": case_environment, "linux": case_linux, "restart": case_restart, "zigbee": case_zigbee}

if __name__ == "__main__":
    work = tempfile.mkdtemp(prefix="hc-service-")
    try:
        CASES[CASE](work)
    except AssertionError as e:
        print(f"FAILED {CASE}: {e}")
        sys.exit(1)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"ok {CASE}")
