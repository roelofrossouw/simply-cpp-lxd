#pragma once

// LXD's REST API (https://documentation.ubuntu.com/lxd/en/latest/rest-api/) over HTTPS with a TLS
// client certificate, as `lxc remote add` sets one up: a cluster's members tried in turn, the
// server known by the cluster's own certificate.
//
//     sc::lxd::client lxd{{"https://10.0.100.3:8443", "https://10.0.105.1:8443"},
//                         {.certificate = "agent.crt", .key = "agent.key", .server_certificate = "cluster.crt"}};
//     for (const auto &each: lxd.instances())
//         std::cout << each.name << " on " << each.location << ": " << each.status << '\n';

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
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

        // The URL of the member that answered last (the first before any did).
        [[nodiscard]] std::string member() const;

        [[nodiscard]] const std::vector<std::string> &members() const { return members_; }

    private:
        std::vector<std::string> members_;
        credentials credentials_;
        std::chrono::seconds timeout_;
        mutable std::mutex mutex_;
        mutable std::size_t answered_ = 0; // members_' index of the member that answered last
    };
}
