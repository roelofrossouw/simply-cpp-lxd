"""A stand-in for an LXD member, for sc-lxd's tests: HTTPS that wants a client certificate,
answering as LXD does. Usage: lxd_server.py PORT SERVER_CERT SERVER_KEY CLIENT_CERT INSTANCES_JSON
(PORT 0: a free one, the system's choice; "listening PORT" on stdout says which).

web1 has a few files (a file, a symlink, a directory); files can be written there (in a folder it
has, or / and /etc) and removed. exec in web1 answers without running
anything: its output is the command joined by spaces, "warned" on stderr, exit status 3 when the
command has "fail" in it; one with "sleep" is still running at the wait's timeout.

Instances can be made (POST /1.0/instances: stopped, on ?target=; 409 for a name there is, a failed
operation for the image alias "broken"), started and stopped (PUT .../state: running with an
address, or stopped), changed (PUT on the instance, with the ETag its GET gave in If-Match: 412
when it's another; a limits.cpu that isn't a number refused, 400) and removed (DELETE, only a
stopped one)."""
import hashlib, http.server, json, ssl, sys, urllib.parse, uuid

port, server_cert, server_key, client_cert, instances_file = sys.argv[1:6]
instances = json.load(open(instances_file))

files = {
    "/etc/hostname": {"type": "file", "mode": "0644", "uid": "0", "gid": "0", "content": b"web1\n"},
    "/etc/sudoers": {"type": "file", "mode": "0440", "uid": "0", "gid": "0", "content": b"root ALL=(ALL) ALL\n\x00"},
    "/etc/localtime": {"type": "symlink", "mode": "0777", "uid": "0", "gid": "0",
                       "content": b"/usr/share/zoneinfo/Africa/Johannesburg"},
    "/etc/apt": {"type": "directory", "mode": "0755", "uid": "0", "gid": "0", "entries": ["sources.list", "sources.list.d"]},
}
operations = {}  # id: the operation's metadata, as /wait gives it
logs = {}        # path: content


class Handler(http.server.BaseHTTPRequestHandler):
    def answer(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def raw(self, body, headers):
        self.send_response(200)
        for name, value in headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def ok(self, metadata):
        self.answer(200, {"type": "sync", "status": "Success", "status_code": 200, "metadata": metadata})

    def missing(self, what="Not Found"):
        self.answer(404, {"type": "error", "error": what, "error_code": 404})

    def operation(self, status="Success", err=""):
        id = str(uuid.uuid4())
        operations[id] = {"id": id, "status": status, "err": err, "metadata": {}}
        self.answer(202, {"type": "async", "status": "Operation created", "status_code": 100,
                          "operation": "/1.0/operations/" + id, "metadata": {"id": id, "class": "task"}})

    def instance(self, name, project):
        return next((each for each in instances if each["name"] == name and each["project"] == project), None)

    @staticmethod
    def etag(found):
        return hashlib.sha256(json.dumps([found.get(key) for key in ("config", "devices", "profiles")], sort_keys=True).encode()).hexdigest()

    def do_PUT(self):
        url = urllib.parse.urlsplit(self.path)
        order = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        project = urllib.parse.parse_qs(url.query).get("project", ["default"])[0]
        if url.path.startswith("/1.0/instances/") and "/" not in url.path[len("/1.0/instances/"):]:
            found = self.instance(urllib.parse.unquote(url.path[len("/1.0/instances/"):]), project)
            if not found:
                return self.missing("Instance not found")
            if self.headers.get("If-Match") not in (None, self.etag(found)):
                return self.answer(412, {"type": "error", "error": "ETag doesn't match", "error_code": 412})
            if not str(order.get("config", {}).get("limits.cpu", "1")).isdigit():
                return self.answer(400, {"type": "error", "error": "Invalid config: limits.cpu", "error_code": 400})
            for key in ("config", "devices", "profiles", "description"):
                if key in order:
                    found[key] = order[key]
            return self.operation()
        if not (url.path.startswith("/1.0/instances/") and url.path.endswith("/state")):
            return self.missing()
        found = self.instance(urllib.parse.unquote(url.path[len("/1.0/instances/"):-len("/state")]), project)
        if not found:
            return self.missing("Instance not found")
        running = order.get("action") in ("start", "restart")
        found["status"], found["status_code"] = ("Running", 103) if running else ("Stopped", 102)
        found["state"] = {"status": found["status"], "status_code": found["status_code"], "memory": {"usage": 0}, "disk": {},
                          "network": {"eth0": {"addresses": [{"family": "inet", "address": "10.0.100.250", "scope": "global"}]}}
                          if running else None}
        self.operation()

    def do_POST(self):
        url = urllib.parse.urlsplit(self.path)
        data = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if url.path == "/1.0/instances/web1/files":
            # A write, as LXD takes one: the bytes, X-LXD-* for what it is. The folder must exist.
            path = urllib.parse.parse_qs(url.query).get("path", [""])[0]
            folder = path.rsplit("/", 1)[0] or "/"
            if folder not in files and folder not in ("/", "/etc"):
                return self.missing("Parent directory not found")
            if self.headers.get("X-LXD-write", "overwrite") != "overwrite":
                return self.answer(400, {"type": "error", "error": "only overwrite in the test", "error_code": 400})
            kind = self.headers.get("X-LXD-type", "file")
            files[path] = {"type": kind, "mode": self.headers.get("X-LXD-mode", "0644"), "uid": self.headers.get("X-LXD-uid", "0"),
                           "gid": self.headers.get("X-LXD-gid", "0"), "content": data, "entries": []}
            return self.ok({})
        order = json.loads(data or b"{}")
        if url.path == "/1.0/instances":
            query = urllib.parse.parse_qs(url.query)
            project = query.get("project", ["default"])[0]
            if self.instance(order.get("name"), project):
                return self.answer(409, {"type": "error", "error": "Instance \"" + order["name"] + "\" already exists", "error_code": 409})
            if order.get("source", {}).get("alias") == "broken":
                return self.operation("Failure", "Failed getting remote image info: image not found")
            instances.append({"name": order["name"], "project": project, "location": query.get("target", ["hv"])[0],
                              "type": order.get("type", "container"), "description": "", "status": "Stopped", "status_code": 102,
                              "created_at": "2026-10-26T12:00:00Z", "last_used_at": "", "config": order.get("config", {}),
                              "devices": order.get("devices", {}), "profiles": order.get("profiles", []),
                              "state": {"status": "Stopped", "status_code": 102, "memory": {"usage": 0}, "disk": {}, "network": None}})
            return self.operation()
        if url.path == "/1.0/instances/web2/exec":
            return self.answer(400, {"type": "error", "error": "Instance is not running", "error_code": 400})
        if url.path != "/1.0/instances/web1/exec":
            return self.missing()
        if order.get("wait-for-websocket") or not order.get("record-output"):
            return self.answer(400, {"type": "error", "error": "the test wants record-output", "error_code": 400})
        command = order["command"]
        id = str(uuid.uuid4())
        if any("sleep" in part for part in command):
            operations[id] = {"id": id, "status": "Running", "err": "", "metadata": {}}
        else:
            base = "/1.0/instances/web1/logs/exec-output/exec_" + id
            text = " ".join(command)
            for name, value in sorted(order.get("environment", {}).items()):
                text += " " + name + "=" + value
            logs[base + ".stdout"] = (text + "\n").encode()
            logs[base + ".stderr"] = b"warned\n"
            code = 3 if any("fail" in part for part in command) else 0
            operations[id] = {"id": id, "status": "Success", "err": "",
                              "metadata": {"output": {"1": base + ".stdout", "2": base + ".stderr"}, "return": code}}
        self.answer(202, {"type": "async", "status": "Operation created", "status_code": 100,
                          "operation": "/1.0/operations/" + id, "metadata": {"id": id, "class": "task"}})

    def do_DELETE(self):
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        if path == "/1.0/instances/web1/files":
            target = urllib.parse.parse_qs(url.query).get("path", [""])[0]
            if target not in files:
                return self.missing()
            del files[target]
            return self.ok({})
        if path in logs:
            del logs[path]
            return self.ok({})
        if path.startswith("/1.0/instances/"):
            found = self.instance(urllib.parse.unquote(path[len("/1.0/instances/"):]),
                                  urllib.parse.parse_qs(url.query).get("project", ["default"])[0])
            if not found:
                return self.missing("Instance not found")
            if found["status_code"] != 102:
                return self.answer(400, {"type": "error", "error": "Instance is running", "error_code": 400})
            instances.remove(found)
            return self.operation()
        self.missing()

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == "/1.0/instances/web1/files":
            found = files.get(query.get("path", [""])[0])
            if not found:
                return self.missing()
            headers = {"X-LXD-type": found["type"], "X-LXD-mode": found["mode"], "X-LXD-uid": found["uid"], "X-LXD-gid": found["gid"]}
            if found["type"] == "directory":
                body = json.dumps({"type": "sync", "status": "Success", "status_code": 200, "metadata": found["entries"]}).encode()
                return self.raw(body, dict(headers, **{"Content-Type": "application/json"}))
            return self.raw(found["content"], dict(headers, **{"Content-Type": "application/octet-stream"}))
        if url.path in logs:
            return self.raw(logs[url.path], {"Content-Type": "application/octet-stream"})
        if url.path.startswith("/1.0/operations/") and url.path.endswith("/wait"):
            id = url.path[len("/1.0/operations/"):-len("/wait")]
            return self.ok(operations[id]) if id in operations else self.missing()
        if url.path == "/1.0/instances" and query.get("recursion") == ["2"]:
            return self.answer(200, {"type": "sync", "status": "Success", "status_code": 200, "metadata": instances})
        if url.path.startswith("/1.0/instances/"):
            name = urllib.parse.unquote(url.path[len("/1.0/instances/"):])
            for each in instances:
                if each["name"] == name and each["project"] == query.get("project", ["default"])[0]:
                    data = json.dumps({"type": "sync", "status": "Success", "status_code": 200, "metadata": each}).encode()
                    return self.raw(data, {"Content-Type": "application/json", "ETag": self.etag(each)})
        self.answer(404, {"type": "error", "error": "Instance not found", "error_code": 404})

    def log_message(self, *args):
        pass


context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(server_cert, server_key)
# TLS 1.2 at most: macOS's libcurl (LibreSSL 3.3) can't decode OpenSSL 4's TLS 1.3 handshake (Homebrew's Python).
context.maximum_version = ssl.TLSVersion.TLSv1_2
context.verify_mode = ssl.CERT_REQUIRED
context.load_verify_locations(client_cert)
server = http.server.HTTPServer(("127.0.0.1", int(port)), Handler)
server.socket = context.wrap_socket(server.socket, server_side=True)
print("listening", server.server_address[1], flush=True)
server.serve_forever()
