# simply-cpp-lxd

[LXD](https://canonical.com/lxd)'s REST API for [simply-cpp](https://github.com/roelofrossouw/simply-cpp): a
cluster's instances and their state, files in them, commands run in them, over HTTPS with a TLS
client certificate.

- **A cluster's members, in turn.** Give every member's URL; a request goes to the member that
  answered last, then the others, so one member down (or rebuilding) changes nothing.
- **Known both ways.** The client proves itself with a certificate the cluster trusts
  (`lxc config trust add`). The server is known by the cluster's own certificate (`lxc info`
  shows it as `environment.certificate`), trusted alone and not checked against the address, which
  a cluster certificate doesn't name; its public key can be pinned too.
- **Instances with their state**, every project at once: name, project, location (the member it's
  on), type, status and LXD's status code, its global IPv4 addresses, memory and root-disk usage,
  created and last used.
- **Files and commands.** `read_file()` gives a file, symlink or directory in an instance with its
  type, mode and owner (nothing when it isn't there). `exec()` runs a command and gives its exit
  status, stdout and stderr: LXD records the output, which is read and then removed there, so no
  websocket is needed.

```cpp
#include <sc_lxd.h>

sc::lxd::client lxd{{"https://10.0.100.3:8443", "https://10.0.105.1:8443"},
                    {.certificate = "/etc/agent/lxd.crt", .key = "/etc/agent/lxd.key",
                     .server_certificate = "/etc/agent/cluster.crt"}};
for (const auto &each: lxd.instances())
    std::cout << each.name << " on " << each.location << ": " << each.status << '\n';
if (const auto web = lxd.find("web1")) std::cout << web->ipv4.front() << '\n';
const auto networks = lxd.get("/1.0/networks?recursion=1"); // anything else, as JSON

if (const auto hosts = lxd.read_file("web1", "/etc/hosts")) std::cout << hosts->content;
const auto listed = lxd.exec("web1", {"sh", "-c", "apt list --upgradable"},
                             {.limit = std::chrono::minutes{5}, .environment = {{"LANG", "C.UTF-8"}}});
if (listed.status == 0) std::cout << listed.out;
```

`get()` returns the answer's `metadata`. `exec()` throws `sc::lxd::error` when LXD refuses (an
instance that isn't running: 400) or the command outlasts its limit (408). Many execs in quick
succession load a cluster's database, and every other request waits on it: space them out. LXD saying no (an unknown instance, a project the
certificate may not see) throws `sc::lxd::error` with its status; no member answering throws
`sc::lxd::unreachable`, naming each member's reason.

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

## Licence

Apache License 2.0; see LICENSE and NOTICE.
