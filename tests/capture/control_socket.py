#!/usr/bin/env python3
"""Real daemon owner checks inside private user/mount namespaces; no host PAM/store writes."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def receive(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise AssertionError("unexpected EOF")
        data += chunk
    return data


def request(path, command, username="root", **extra):
    payload = dict(version=2, msg_type="captures", request_id=123,
                   username=username, command=command)
    payload.update(extra)
    data = json.dumps(payload).encode()
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        sock.connect(str(path))
        sock.sendall(struct.pack("!I", len(data)) + data)
        size, = struct.unpack("!I", receive(sock, 4))
        return json.loads(receive(sock, size))


def run(daemon):
    subprocess.run(["mount", "-t", "tmpfs", "-o", "mode=700", "tmpfs", "/var/lib"], check=True)
    with tempfile.TemporaryDirectory(prefix="s2u-capture-control-") as temporary:
        root = Path(temporary)
        key = root / "smile2unlock-master.key"
        key.write_bytes(b"S2UK\0\1\2\0" + struct.pack("!I", 1) + bytes([42]) * 32)
        key.chmod(0o600)
        path = root / "control.sock"
        env = dict(os.environ, CREDENTIALS_DIRECTORY=str(root))
        with open(root / "daemon.log", "w") as log:
            process = subprocess.Popen([daemon, str(path)], env=env, stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if path.exists():
                        break
                    assert process.poll() is None, "daemon exited early"
                    time.sleep(0.05)
                configured = request(path, dict(op="configure", settings=dict(enabled=True, days=7, max_count=50)))
                assert configured["result"] == "accepted", configured
                assert request(path, dict(op="list"))["result"] == "accepted"
                assert request(path, dict(op="delete_all"))["result"] == "accepted"
                foreign = os.environ["S2U_CAPTURE_FOREIGN_USER"]
                assert request(path, dict(op="list"), username=foreign)["result"] == "rejected"
                assert request(path, dict(op="delete_all"), username=foreign)["result"] == "rejected"
                for command in [dict(op="store"), dict(op="fetch", id="../profiles.s2u"),
                                dict(op="configure", settings=dict(enabled=True, days=0, max_count=50)),
                                dict(op="configure", settings=dict(enabled=True, days=7, max_count=201)),
                                dict(op="list", unexpected=True)]:
                    assert request(path, command)["result"] != "accepted", command
                assert request(path, dict(op="list"), version=99)["result"] == "rejected"
                assert request(path, dict(op="list"), unexpected=True)["result"] == "rejected"
                store = Path("/var/lib/smile2unlock/users/0/captures")
                assert store.is_dir() and store.stat().st_mode & 0o777 == 0o700
                print("PASS: real su_authd configure/list/delete-all, strict peer uid (including root), bounds and malformed operations")
            finally:
                process.terminate()
                process.wait(timeout=5)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--inside":
        run(sys.argv[2])
    else:
        import pwd
        env = dict(os.environ, S2U_CAPTURE_FOREIGN_USER=pwd.getpwuid(os.getuid()).pw_name)
        subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "--fork",
                        sys.executable, __file__, "--inside", str(Path(sys.argv[1]).resolve())],
                       check=True, env=env)
