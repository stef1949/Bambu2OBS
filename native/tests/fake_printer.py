"""A local stand-in for a Bambu printer, used only by the native integration test.

It speaks enough MQTT over TLS and implicit FTPS to exercise the plugin's real
network code, using a throwaway self-signed certificate. Like real printers, its
FTPS server rejects data connections that do not resume the control TLS session,
so a client that loses session reuse fails here instead of on hardware.
"""

import argparse
import hashlib
import io
import json
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import zipfile
import zlib

ACCESS_CODE = "12345678"
SERIAL = "TESTSERIAL0001"

FULL_STATUS = {
    "print": {
        "gcode_state": "RUNNING",
        "subtask_name": "Fake Benchy",
        "task_id": "7001",
        "print_type": "local",
        "gcode_file": "Fake Benchy.gcode.3mf",
        "mc_percent": 42,
        "mc_remaining_time": 83,
        "layer_num": 120,
        "total_layer_num": 350,
        "nozzle_temper": 219.5,
        "bed_temper": 60,
        "cooling_fan_speed": "12",
        "spd_lvl": 2,
        "stg_cur": 0,
        "ams": {
            "tray_now": "0",
            "ams": [
                {
                    "id": "0",
                    "tray": [
                        {
                            "id": "0",
                            "tray_info_idx": "GFA00",
                            "tray_type": "PLA",
                            "tray_color": "FF0000FF",
                        }
                    ],
                }
            ],
        },
    }
}


def png_bytes():
    """A 4x4 RGBA PNG whose first pixel is pure red."""

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    rows = b""
    for _ in range(4):
        rows += b"\x00" + b"\xff\x00\x00\xff" * 4
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def archive_bytes():
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("3D/3dmodel.model", "<model/>" * 200)
        archive.writestr("Metadata/plate_1.png", png_bytes())
    return buffer.getvalue()


def make_certificate(openssl, directory):
    certificate = os.path.join(directory, "printer.pem")
    key = os.path.join(directory, "printer.key")
    subprocess.run(
        [
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-subj", f"/CN={SERIAL}", "-keyout", key, "-out", certificate,
        ],
        check=True,
        capture_output=True,
    )
    der = ssl.PEM_cert_to_DER_cert(open(certificate, encoding="ascii").read())
    return certificate, key, hashlib.sha256(der).hexdigest()


def tls_context(certificate, key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(certificate, key)
    return context


def encode_length(length):
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        out.append(byte | (0x80 if length else 0))
        if not length:
            return bytes(out)


def publish_packet(topic, payload):
    body = struct.pack(">H", len(topic)) + topic.encode() + payload
    return b"\x30" + encode_length(len(body)) + body


class Broker:
    def __init__(self, context):
        self.context = context
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.clients = []
        self.logins = 0
        self.lock = threading.Lock()
        self.status = json.loads(json.dumps(FULL_STATUS))
        threading.Thread(target=self.accept_loop, daemon=True).start()

    def accept_loop(self):
        while True:
            raw, _ = self.server.accept()
            threading.Thread(target=self.serve, args=(raw,), daemon=True).start()

    def broadcast(self, payload):
        packet = publish_packet(f"device/{SERIAL}/report", payload)
        with self.lock:
            for client in list(self.clients):
                try:
                    client.sendall(packet)
                except OSError:
                    self.clients.remove(client)

    def drop_all(self):
        with self.lock:
            for client in self.clients:
                try:
                    client.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                client.close()
            self.clients.clear()

    def read_packet(self, connection):
        header = connection.recv(1)
        if not header:
            return None, None
        length = 0
        shift = 0
        while True:
            byte = connection.recv(1)[0]
            length |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        body = b""
        while len(body) < length:
            part = connection.recv(length - len(body))
            if not part:
                return None, None
            body += part
        return header[0] >> 4, body

    def serve(self, raw):
        try:
            connection = self.context.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError):
            return
        try:
            while True:
                kind, body = self.read_packet(connection)
                if kind is None:
                    return
                if kind == 1:  # CONNECT
                    self.logins += 1
                    offset = 10
                    fields = []
                    for _ in range(3):
                        size = struct.unpack(">H", body[offset:offset + 2])[0]
                        fields.append(body[offset + 2:offset + 2 + size].decode())
                        offset += 2 + size
                    accepted = fields[1] == "bblp" and fields[2] == ACCESS_CODE
                    connection.sendall(b"\x20\x02\x00" + (b"\x00" if accepted else b"\x05"))
                    if not accepted:
                        return
                elif kind == 8:  # SUBSCRIBE
                    connection.sendall(b"\x90\x03" + body[:2] + b"\x00")
                    with self.lock:
                        self.clients.append(connection)
                elif kind == 3:  # PUBLISH: the client's pushall request
                    payload = json.dumps(self.status).encode()
                    connection.sendall(publish_packet(f"device/{SERIAL}/report", payload))
                elif kind == 12:  # PINGREQ
                    connection.sendall(b"\xd0\x00")
                elif kind == 14:  # DISCONNECT
                    return
        except (OSError, ssl.SSLError, IndexError):
            pass
        finally:
            with self.lock:
                if connection in self.clients:
                    self.clients.remove(connection)
            try:
                connection.close()
            except OSError:
                pass


class Ftps:
    def __init__(self, context):
        self.context = context
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.archive = archive_bytes()
        self.retr_count = 0
        threading.Thread(target=self.accept_loop, daemon=True).start()

    def accept_loop(self):
        while True:
            raw, _ = self.server.accept()
            threading.Thread(target=self.serve, args=(raw,), daemon=True).start()

    def listing(self, path):
        if path == "cache":
            return "-rw-r--r-- 1 root root 77 Jan  3 2025 older.3mf\r\n"
        return (
            f"-rw-r--r-- 1 root root {len(self.archive)} Mar 14 09:30 Fake Benchy.gcode.3mf\r\n"
            "drwxr-xr-x 2 root root 4096 Jan  2 2024 cache\r\n"
            "-rw-r--r-- 1 root root 12 Jan  2 2024 notes.txt\r\n"
        )

    def serve(self, raw):
        try:
            control = self.context.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError):
            return
        reader = control.makefile("rb")
        data_server = None
        authenticated = False

        def reply(text):
            control.sendall(text.encode() + b"\r\n")

        def send_data(payload):
            connection, _ = data_server.accept()
            tls = self.context.wrap_socket(connection, server_side=True)
            if not tls.session_reused:
                # The behaviour of real printers, which this test exists to pin down.
                tls.close()
                reply("425 Data channel must reuse the control session")
                return
            reply("150 Opening data connection")
            tls.sendall(payload)
            # OpenSSL forgets a session whose socket closes without a TLS shutdown,
            # which would make the next data channel unable to resume.
            tls.unwrap().close()
            reply("226 Transfer complete")

        try:
            reply("220-Fake printer")
            reply("220 Ready")
            while True:
                line = reader.readline()
                if not line:
                    return
                command, _, argument = line.decode().strip().partition(" ")
                command = command.upper()
                if command == "USER":
                    reply("331 Password required")
                elif command == "PASS":
                    authenticated = argument == ACCESS_CODE
                    reply("230 Logged in" if authenticated else "530 Login incorrect")
                elif not authenticated:
                    reply("530 Not logged in")
                elif command in ("PBSZ", "PROT", "TYPE"):
                    reply("200 OK")
                elif command == "PASV":
                    data_server = socket.create_server(("127.0.0.1", 0))
                    port = data_server.getsockname()[1]
                    reply(f"227 Entering Passive Mode (127,0,0,1,{port // 256},{port % 256})")
                elif command == "LIST":
                    send_data(self.listing(argument).encode())
                elif command == "RETR":
                    if argument != "Fake Benchy.gcode.3mf":
                        reply("550 Not found")
                    else:
                        self.retr_count += 1
                        send_data(self.archive)
                elif command == "QUIT":
                    reply("221 Bye")
                    return
                else:
                    reply("502 Not implemented")
        except (OSError, ssl.SSLError):
            pass
        finally:
            control.close()


def control_loop(server, broker, ftps):
    while True:
        connection, _ = server.accept()
        with connection:
            command = connection.makefile("r").readline().strip()
            if command.startswith("delta "):
                percent = int(command.split()[1])
                broker.broadcast(json.dumps({"print": {"mc_percent": percent}}).encode())
            elif command == "garbage":
                broker.broadcast(b"{not json")
            elif command == "drop":
                broker.drop_all()
            elif command == "newjob":
                broker.broadcast(
                    json.dumps({"print": {"task_id": "7002", "subtask_name": "Other job"}}).encode()
                )
            elif command == "oldjob":
                broker.status = json.loads(json.dumps(FULL_STATUS))
            elif command == "reset-ftps-stats":
                ftps.retr_count = 0
            elif command == "logins":
                connection.sendall(f"{broker.logins}\n".encode())
                continue
            connection.sendall(b"ok\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--client", required=True)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory() as directory:
        certificate, key, fingerprint = make_certificate(arguments.openssl, directory)
        context = tls_context(certificate, key)
        broker = Broker(context)
        ftps = Ftps(context)
        control = socket.create_server(("127.0.0.1", 0))
        threading.Thread(target=control_loop, args=(control, broker, ftps), daemon=True).start()

        result = subprocess.run(
            [
                arguments.client,
                str(broker.port),
                str(ftps.port),
                str(control.getsockname()[1]),
                fingerprint,
                directory,
            ],
            timeout=170,
            check=False,
        )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
