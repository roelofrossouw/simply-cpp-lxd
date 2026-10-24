"""A stand-in for an LXD member, for sc-lxd's tests: HTTPS that wants a client certificate,
answering as LXD does. Usage: lxd_server.py PORT SERVER_CERT SERVER_KEY CLIENT_CERT INSTANCES_JSON"""
import http.server, json, ssl, sys, urllib.parse

port, server_cert, server_key, client_cert, instances_file = sys.argv[1:6]
instances = json.load(open(instances_file))


class Handler(http.server.BaseHTTPRequestHandler):
    def answer(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == "/1.0/instances" and query.get("recursion") == ["2"]:
            return self.answer(200, {"type": "sync", "status": "Success", "status_code": 200, "metadata": instances})
        if url.path.startswith("/1.0/instances/"):
            name = urllib.parse.unquote(url.path[len("/1.0/instances/"):])
            for each in instances:
                if each["name"] == name and each["project"] == query.get("project", ["default"])[0]:
                    return self.answer(200, {"type": "sync", "status": "Success", "status_code": 200, "metadata": each})
        self.answer(404, {"type": "error", "error": "Instance not found", "error_code": 404})

    def log_message(self, *args):
        pass


context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(server_cert, server_key)
context.verify_mode = ssl.CERT_REQUIRED
context.load_verify_locations(client_cert)
server = http.server.HTTPServer(("127.0.0.1", int(port)), Handler)
server.socket = context.wrap_socket(server.socket, server_side=True)
print("listening", flush=True)
server.serve_forever()
