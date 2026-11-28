# simply-cpp-lxd

[LXD](https://canonical.com/lxd)'s REST API for [simply-cpp](https://github.com/roelofrossouw/simply-cpp): a
cluster's instances and their state, files in them, commands run in them, over HTTPS with a TLS
client certificate.

- **A cluster's members, in turn.** Give every member's URL; a request goes to the member that
  answered last, then the others, so one member down (or rebuilding) changes nothing. A request that
  times out reading its answer isn't asked of the others (since 6.0.0): each would only hand the same
  work to the same busy member.
- **Calls go where the work is** (since 6.0.0). A call about an instance (its state, files, `exec()`,
  config, devices) and its operations go to the member that has it, so no member forwards them to
  another; a member that kept forwarding cluster-wide listings to a slow one clogged that one's
  database for hours. The members (`/1.0/cluster/members`) are read every 10 minutes (every 30 s
  while one isn't Online: an evacuated member is taken back soon after it's restored), the instances'
  members (a listing without state) every 5 minutes or when one is new. `list()` takes the names (from
  the database, nothing forwarded), then reads each instance's state on its own member, the members
  side by side and 4 at a time on each; after a state read fails on a member its other instances
  aren't asked this time (`state_read` false: status as the database had it, no usage), and
  `listing::members` says how each member answered (`GET /1.0` and the slowest state read, in ms).
- **Known both ways.** The client proves itself with a certificate the cluster trusts
  (`lxc config trust add`). The server is known by the cluster's own certificate (`lxc info`
  shows it as `environment.certificate`), trusted alone and not checked against the address, which
  a cluster certificate doesn't name; its public key can be pinned too.
- **Instances with their state**, every project at once: name, project, location (the member it's
  on), type, status and LXD's status code, its global IPv4 addresses, memory and root-disk usage,
  created and last used.
- **Files and commands.** `read_file()` gives a file, symlink or directory in an instance with its
  type, mode and owner (nothing when it isn't there); `write_file()` writes one, with its owner and
  mode, replacing what's there (since 1.2.0), and `delete_file()` removes one. `exec()` runs a command and gives its exit
  status, stdout and stderr: LXD records the output, which is read and then removed there, so no
  websocket is needed.
- **Instances made, started and removed** (since 1.3.0). `create()` makes one from a body as LXD
  takes it (name, type, source, profiles, config, devices), on a cluster member of your choice, and
  waits for it, an image's download included; `change_state()` starts, stops or restarts one and
  waits; `remove()` removes one, stopping it first when it runs. Each throws `sc::lxd::error`: 409
  for a name that's taken, 500 when LXD's operation fails, 408 when it's still going at the limit.
- **Config keys set** (since 1.4.0). `set_config()` sets some keys of an instance's own config and
  keeps the rest, its devices and profiles: read, changed and put back, guarded by LXD's ETag (read
  again when the instance changed meanwhile). 400 for a value LXD refuses. `remove_device()`
  (since 1.6.0) takes a device out of an instance's own config the same way and gives what it was;
  `set_device()` (since 1.7.0) adds or replaces one.
- **Standalone servers beside the cluster** (since 4.0.0). A host that isn't a member, with its own
  URL and certificate, given by name: a call about an instance goes to the server that has it (pinned
  with `place()`, else as the servers' own listings say, read again at most every 30 s), `create()`
  with the host's name as its target makes the instance there, and `instances()` lists the cluster's
  and theirs, a standalone instance's location being the host's name. `list()` also names a standalone
  host that didn't answer (its instances are left out, not the whole listing); its pinned instances'
  calls fail rather than go to the cluster. Without standalone servers the client is the cluster's
  alone, exactly as before.

```cpp
#include <sc/lxd/lxd.h>

sc::lxd::client lxd{{"https://10.0.100.3:8443", "https://10.0.105.1:8443"},
                    {.certificate = "/etc/agent/lxd.crt", .key = "/etc/agent/lxd.key",
                     .server_certificate = "/etc/agent/cluster.crt"}};
for (const auto &each: lxd.instances())
    std::cout << each.name << " on " << each.location << ": " << each.status << '\n';
if (const auto web = lxd.find("web1")) std::cout << web->ipv4.front() << '\n';
const auto networks = lxd.get("/1.0/networks?recursion=1"); // anything else, as JSON

if (const auto hosts = lxd.read_file("web1", "/etc/hosts")) std::cout << hosts->content;
lxd.write_file("web1", "/etc/motd", {.type = "file", .content = "Hello\n", .mode = 0644}); // root's, 644
lxd.delete_file("web1", "/etc/motd");                                                   // false: nothing there
const auto listed = lxd.exec("web1", {"sh", "-c", "apt list --upgradable"},
                             {.limit = std::chrono::minutes{5}, .environment = {{"LANG", "C.UTF-8"}}});
if (listed.status == 0) std::cout << listed.out;
// As another user (by number) in a folder; LXD sets no HOME for it.
lxd.exec("web1", {"git", "status"}, {.environment = {{"HOME", "/var/www"}}, .user = 33, .group = 33, .folder = "/var/www/html"});

lxd.create({{"name", "web3"}, {"type", "container"}, {"profiles", {"default"}},
            {"source", {{"type", "image"}, {"protocol", "simplestreams"},
                        {"server", "https://cloud-images.ubuntu.com/releases"}, {"alias", "noble"}}}},
           "default", "hv2");                                                         // on member hv2
lxd.change_state("web3", "start");
lxd.set_config("web3", {{"limits.cpu", "4"}, {"limits.memory", "8GiB"}});            // the rest kept
lxd.remove("web3");                                                                   // stopped first
```

`get()` returns the answer's `metadata`. `exec()` throws `sc::lxd::error` when LXD refuses (an
instance that isn't running: 400) or the command outlasts its limit (408). Many execs in quick
succession load a cluster's database, and every other request waits on it: space them out. An
exec takes four requests one after another (since 1.7.1 its stderr is read and removed beside its
stdout, not after it). LXD saying no (an unknown instance, a project the
certificate may not see) throws `sc::lxd::error` with its status; no member answering throws
`sc::lxd::unreachable`, naming each member's reason.

With a standalone server (a host that left the cluster, say), the same client certificate is
trusted there too (`lxc config trust add` on it), and its own certificate is given:

```cpp
sc::lxd::client lxd{{"https://10.0.100.3:8443"}, {.certificate = "lxd.crt", .key = "lxd.key", .server_certificate = "cluster.crt"},
                    {{.name = "hv3", .urls = {"https://192.0.2.5:8443"},
                      .how = {.certificate = "lxd.crt", .key = "lxd.key", .server_certificate = "hv3.crt"}}}};
lxd.place("default", "mail3", "hv3");   // what a design says lives there
lxd.exec("mail3", {"postqueue", "-p"}); // to hv3
const auto all = lxd.list();            // all.unanswered: standalone hosts that didn't answer
```

Calls can be paced (since 6.1.0), for the whole process and every client, by member URL:

```cpp
sc::lxd::set_pacing({.in_flight = 2,            // at most two calls under way on a member
                     .listing_gap = 2s,         // instance listings (GET /1.0/instances) that far apart
                     .slow = 3s,                // an answer slower than this: the member backs off
                     .max_backoff = 30s,        // for as long as it took, at most this
                     .max_wait = 60s});         // a call that waited this long goes ahead anyway
for (const auto &member: sc::lxd::member_queues()) // in flight, waiting, calls, waited, backoffs, overdue
    std::cout << member.url << " " << member.waiting << "\n";
```

Waits for an operation (`/1.0/operations/<id>/wait`) aren't paced. `in_flight` 0, the default, paces nothing.

A TLS client on LXD 5.21 can't be made read only (`--restricted --projects` only limits projects),
so the key is as powerful as root on the cluster: keep it on the hosts that use it, mode 600.

## Install

```sh
curl -fsSL https://apt.roelof.co.za/setup.sh | bash
sudo apt install simply-cpp-lxd-dev
```

In CMake: `find_package(sc-lxd)` and link `sc::sc-lxd` (or `sc::sc-lxd-shared`).

`sc-lxd-demo CERT KEY SERVER_CERT MEMBER_URL...` lists the instances; with
`--file web1:/etc/hosts` it shows a file, with `--exec web1 --command "uptime"` it runs one.

## Tests

`test-instances` reads a recorded answer; `test-client` runs a stand-in member
(`tests/resource/lxd_server.py`, HTTPS wanting a client certificate, with openssl-made certificates):
failover, an unknown instance, LXD's errors, files (a file, a symlink, a directory, a missing
path), exec (output, exit status, refused, too slow), an unknown client and an untrusted server.
`test-standalone` runs two (a cluster and a standalone host with its own certificate): listings
merged, calls routed by listing and by pin, made and removed on the standalone host, one that
doesn't answer.

## Licence

Apache License 2.0; see LICENSE and NOTICE.
