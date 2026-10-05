"""Exercise the real C HTTP server on loopback; no PS5 connection is made."""
import json
import pathlib
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import urllib.parse

binary, config_dir = sys.argv[1:3]
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
base = f"http://127.0.0.1:{port}"
process = subprocess.Popen([binary, "--serve", str(port)], stdout=subprocess.DEVNULL)

def get(path, expected=200):
    try:
        response = urllib.request.urlopen(base + path, timeout=3)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        assert response.status == expected, (path, response.status, data)
        assert int(response.headers["Content-Length"]) == len(data)
        assert response.headers["Cache-Control"] == "no-store"
        return data

def post(path, values, expected=200):
    body = urllib.parse.urlencode(values).encode()
    req = urllib.request.Request(base + path, data=body, headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        response = urllib.request.urlopen(req, timeout=3)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        assert response.status == expected, (path, response.status, data)
        return json.loads(data)

def backup_form(backup):
    result = {"version": backup["version"]}
    result.update({"cfg." + k: v for k, v in backup["config"].items()})
    for i, profile in enumerate(backup["profiles"], 1):
        prefix = f"p{i}."
        result[prefix + "name"] = profile["name"] if profile else ""
        if profile:
            result.update({prefix + k: v for k, v in profile["config"].items()})
    return result

def raw(parts):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        for part in parts:
            sock.sendall(part)
            time.sleep(0.01)
        data = b""
        try:
            while chunk := sock.recv(65536):
                data += chunk
        except ConnectionResetError:
            # Oversized requests can leave unread bytes when the server closes.
            assert data, "Connection reset before the HTTP response"
        return data

try:
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                break
        except OSError:
            time.sleep(0.03)
    else:
        raise RuntimeError("Test server did not start")
    config = json.loads(get("/api/config"))
    duplicate = subprocess.run([binary, "--serve", str(port)], timeout=3, capture_output=True)
    assert duplicate.returncode == 2, duplicate
    assert json.loads(get("/api/config")) == config
    assert config["deadzone"] == 368
    telemetry = json.loads(get("/api/telemetry"))
    assert not telemetry["connected"] and telemetry["gear"] == 1
    fragmented = raw([b"GET /api/", b"config HTTP/1.1\r\nHost: localhost\r\n", b"\r\n"])
    assert b"200 OK" in fragmented and b'"deadzone":368' in fragmented
    for path in ("/unknown", "/api/config-extra", "/overlay-extra", "/api/telemetry-extra"):
        get(path, 404)
    assert b"400 Bad Request" in raw([b"POST /api/save HTTP/1.1\r\nHost: localhost\r\n\r\n"])
    assert b"400 Bad Request" in raw([b"GET / HTTP/invalid\r\n\r\n"])
    assert b"431 Request Header Fields Too Large" in raw([b"GET / HTTP/1.1\r\nX: " + b"x" * 4080])
    for query in ("deadzone=-1", "deadzone=2001", "deadzone=1x", "btn_a=3", "invert_pedals=2", "unknown=0", "deadzone=777&btn_a=-1"):
        get("/api/save?" + query, 400)
    assert json.loads(get("/api/config")) == config
    assert json.loads(get("/api/save?deadzone=368"))["status"] == "ok"
    obstruction = pathlib.Path(config_dir) / "wheel_config.txt.tmp"
    obstruction.mkdir()
    try:
        assert "error" in json.loads(get("/api/save?deadzone=999", 500))
        assert json.loads(get("/api/config"))["deadzone"] == 368
    finally:
        obstruction.rmdir()
    assert json.loads(get("/api/save?deadzone=777&invert_pedals=1"))["status"] == "ok"
    assert json.loads(get("/api/config"))["deadzone"] == 777
    post("/api/save", {"sensitivity": 125, "curve": 1, "smoothing_ms": 10})
    current = json.loads(get("/api/config"))
    for i in range(1, 9):
        post("/api/profiles/save", {**current, "slot": i, "name": f'Jogo {i} ação "\\', "sensitivity": 100 + i})
    backup = json.loads(get("/api/backup"))
    assert backup["format"] == "ps5-wheel-backup" and len(backup["profiles"]) == 8
    assert json.loads(get("/api/config")) == current
    post("/api/profiles/load", {"slot": 8})
    assert json.loads(get("/api/config"))["sensitivity"] == 108
    post("/api/reset", {"reset": 1})
    assert json.loads(get("/api/config")) == config
    assert json.loads(get("/api/profiles"))["profiles"] == backup["profiles"]
    post("/api/import", backup_form(backup))
    assert json.loads(get("/api/backup")) == backup
    bad_backup = backup_form(backup)
    bad_backup["p8.steer_min"] = bad_backup["p8.steer_center"]
    post("/api/import", bad_backup, 400)
    assert json.loads(get("/api/backup")) == backup
    bad_backup = backup_form(backup); del bad_backup["cfg.sensitivity"]
    post("/api/import", bad_backup, 400)
    obstruction.mkdir()
    try:
        post("/api/import", backup_form(backup), 500)
        assert json.loads(get("/api/backup")) == backup
    finally:
        obstruction.rmdir()
    for name in ("   ", "a\n[profile2]", "a" * 49):
        post("/api/profiles/save", {"slot": 1, "name": name}, 400)
    for body in (b"deadzone=1&deadzone=2", b"deadzone=368%00evil", b"deadzone=368\x00evil", b"slot=1&name=%C0%AF"):
        route = b"/api/profiles/save" if body.startswith(b"slot") else b"/api/save"
        header = b"POST " + route + b" HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n"
        assert b"400 Bad Request" in raw([header, body])
    body = b"sensitivity=130&curve=2"
    header = b"POST /api/save HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n"
    assert b"200 OK" in raw([header[:30], header[30:], body[:9], body[9:]])
    post("/api/gear/reset", {"gear": -1}); assert json.loads(get("/api/telemetry"))["gear"] == -1
    post("/api/gear/reset", {"gear": 9}, 400)
    post("/api/profiles/delete", {"slot": 8}); post("/api/profiles/load", {"slot": 8}, 404)
    assert json.loads(get("/api/profiles"))["profiles"][7] is None
    assert json.loads(get("/api/diagnostics"))["injection_attempts"] == 0
    get("/api/import", 405)
    for route in ("/", "/overlay"):
        assert get(route).startswith(b"<!DOCTYPE html>")
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.sendall(b"GET /overlay HTTP/1.1\r\nHost: localhost\r\n\r\n")
    assert get("/api/config") and process.poll() is None
    print("PASS: profiles, export/import, malformed backups, POST fragments, gear reset, defaults, diagnostics; HTTP fragments, exact routes, response lengths, validation, atomic saves, save failures, closed peers")
finally:
    process.terminate()
    process.wait(timeout=3)
