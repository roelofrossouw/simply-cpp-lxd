#pragma once

// LXD's REST API (https://documentation.ubuntu.com/lxd/en/latest/rest-api/) over HTTPS with a TLS
// client certificate, as `lxc remote add` sets one up: a cluster's members tried in turn, the
// server known by the cluster's own certificate.
//
//     sc::lxd::client lxd{{"https://10.0.100.3:8443", "https://10.0.105.1:8443"},
//                         {.certificate = "agent.crt", .key = "agent.key", .server_certificate = "cluster.crt"}};
//     for (const auto &each: lxd.instances())
//         std::cout << each.name << " on " << each.location << ": " << each.status << '\n';
//     if (const auto hosts = lxd.read_file("web1", "/etc/hosts")) std::cout << hosts->content;
//     lxd.write_file("web1", "/etc/motd", {.type = "file", .content = "Hello\n", .mode = 0644});
//     const auto listed = lxd.exec("web1", {"dpkg-query", "-W"});
//     if (listed.status == 0) std::cout << listed.out;

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sc::lxd {
    // How the client proves who it is, and knows the server.
    struct credentials {
        // The client's certificate and key (PEM), the ones the cluster trusts (lxc config trust add).
        std::filesystem::path certificate;
        std::filesystem::path key;
        // The server's own certificate (PEM; a cluster's members share one: `lxc info` shows it as
        // environment.certificate): trusted alone, and not checked against the address, which a
        // cluster certificate doesn't name. Empty: the system's authorities, the address checked.
        std::filesystem::path server_certificate;
        // Optionally also the server's public key, curl's "sha256//<base64>" (several joined by ';').
        std::string pinned_key;
    };

    // An answer of LXD's own saying no: its error text and status (404 for an instance that
    // doesn't exist, 403 for a project the certificate may not see).
    class error : public std::runtime_error {
    public:
        error(const std::string &message, const int status) : std::runtime_error(message), status_(status) {}

        [[nodiscard]] int status() const { return status_; }

    private:
        int status_;
    };

    // No member answered (each one's reason in what()).
    class unreachable : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // An instance (a container or a virtual machine) with its state.
    struct instance {
        std::string name;
        std::string project;
        std::string location;    // the cluster member it's on ("none" outside a cluster)
        std::string type;        // "container" or "virtual-machine"
        std::string description;
        std::string status;      // "Running", "Stopped", "Frozen", ...
        int status_code = 0;     // LXD's: 103 Running, 102 Stopped, 110 Frozen, 112 Error, ...
        std::vector<std::string> ipv4;          // its global IPv4 addresses, every interface but lo
        std::optional<std::int64_t> memory_usage; // bytes; nothing when it isn't running
        std::optional<std::int64_t> disk_usage;   // its root disk's bytes, when the storage tells
        std::string created_at;   // RFC 3339, as LXD gives them
        std::string last_used_at;

        // From an instance as GET /1.0/instances?recursion=2 lists it (with "state"); without
        // state, nothing that comes from it is filled in.
        [[nodiscard]] static instance from_json(const nlohmann::json &metadata);
    };

    // A path in an instance, as LXD's file API gives it.
    struct file {
        std::string type;                 // "file", "directory" or "symlink"
        std::string content;              // a file's bytes, a symlink's target
        std::vector<std::string> entries; // a directory's names
        int uid = 0;
        int gid = 0;
        unsigned mode = 0;                // the permission bits, 0644; 04755 with setuid
    };

    // What exec() runs a command with.
    struct exec_options {
        std::string project = "default";
        // How long it may take: lxd::error (status 408) when it's still running then.
        std::chrono::seconds limit{60};
        // Its environment besides LXD's own (PATH, HOME, ...): {"LANG", "C.UTF-8"}.
        std::map<std::string, std::string> environment;
    };

    // How a command ended, and what it wrote.
    struct exec_result {
        int status = 0; // its exit status
        std::string out;
        std::string err;
    };

    class client {
    public:
        // members: the cluster's member URLs ("https://10.0.100.3:8443"), tried in this order
        // after the one that answered last. timeout: a request's limit, per member.
        client(std::vector<std::string> members, credentials how, std::chrono::seconds timeout = std::chrono::seconds{10});

        // The metadata of GET path ("/1.0/instances?recursion=2"). Throws lxd::error for an
        // answer of LXD's saying no, lxd::unreachable when no member answers (or one answers with
        // what isn't LXD's JSON).
        [[nodiscard]] nlohmann::json get(std::string_view path) const;

        // Every instance with its state, of every project (or the default one only).
        [[nodiscard]] std::vector<instance> instances(bool all_projects = true) const;

        // One instance with its state; nothing when there's no such instance.
        [[nodiscard]] std::optional<instance> find(std::string_view name, std::string_view project = "default") const;

        // A file, directory or symlink in an instance (a link itself, not where it points);
        // nothing when the path isn't there.
        [[nodiscard]] std::optional<file> read_file(std::string_view instance, std::string_view path,
                                                    std::string_view project = "default") const;

        // Writes a file (its bytes) or a symlink (its target, in content) at path, owned by uid and
        // gid with mode's permission bits (a symlink's mode isn't used), replacing what's there; or
        // makes a directory. The folder it's in must exist. Throws lxd::error when LXD refuses
        // (403: a project the certificate may not use; 404: no such instance or folder).
        void write_file(std::string_view instance, std::string_view path, const file &what, std::string_view project = "default") const;

        // Removes a file, symlink or empty directory; false when there was nothing at path.
        bool delete_file(std::string_view instance, std::string_view path, std::string_view project = "default") const;

        // Runs command in a running instance and waits for it to end: argv as is, no shell
        // unless it's one ({"sh", "-c", "..."}). Its output is recorded by LXD, read, and removed
        // there. Throws lxd::error when LXD refuses (an instance that isn't running: 400) or the
        // command outlasts the limit.
        [[nodiscard]] exec_result exec(std::string_view instance, const std::vector<std::string> &command,
                                       const exec_options &options = {}) const;

        // Makes an instance: POST /1.0/instances with body as LXD takes it (name, type, source,
        // profiles, config, devices), on the cluster member target (empty: LXD's choice), and
        // waits until it's made, an image's download included, or limit passes. Throws lxd::error
        // when LXD refuses (409: an instance of that name exists) or making it fails (500), 408
        // when it's still going at the limit.
        void create(const nlohmann::json &body, std::string_view project = "default", std::string_view target = {},
                    std::chrono::seconds limit = std::chrono::minutes{10}) const;

        // Starts, stops or restarts an instance (action "start", "stop", "restart"; force: a stop
        // that doesn't wait for it to shut down) and waits until it's done or limit passes.
        // Throws lxd::error as create() does.
        void change_state(std::string_view instance, std::string_view action, std::string_view project = "default",
                          bool force = false, std::chrono::seconds limit = std::chrono::minutes{2}) const;

        // Removes an instance, stopping it first (forced) when it runs; false when there's none.
        bool remove(std::string_view instance, std::string_view project = "default",
                    std::chrono::seconds limit = std::chrono::minutes{2}) const;

        // The URL of the member that answered last (the first before any did).
        [[nodiscard]] std::string member() const;

        [[nodiscard]] const std::vector<std::string> &members() const { return members_; }

    private:
        struct response {
            long status = 0;
            std::string body;
            std::map<std::string, std::string> headers;
        };

        // The first member's answer that is LXD's (its JSON, or with raw a file's bytes), from the
        // one that answered last; lxd::unreachable when none answers. body: a POST of it.
        // headers: the request's own besides the certificate's (a file's X-LXD-*, its Content-Type).
        // method: GET, POST, PUT (body: what it sends) or DELETE.
        [[nodiscard]] response request(const char *method, std::string_view path, const std::string *body = nullptr,
                                       bool raw = false, std::chrono::seconds timeout = std::chrono::seconds{0},
                                       const std::map<std::string, std::string> *headers = nullptr) const;
        // The metadata of LXD's JSON answer, or its lxd::error.
        [[nodiscard]] static nlohmann::json metadata_of(const response &answer);
        // Waits for a background operation LXD started (its metadata, with its id) to end;
        // lxd::error when it failed (500) or is still running at limit (408). what names it.
        void wait_for(const nlohmann::json &operation, std::string_view project, std::chrono::seconds limit, const std::string &what) const;

        std::vector<std::string> members_;
        credentials credentials_;
        std::chrono::seconds timeout_;
        mutable std::mutex mutex_;
        mutable std::size_t answered_ = 0; // members_' index of the member that answered last
    };
}
