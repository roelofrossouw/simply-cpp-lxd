#include "sc_lxd.h"

#include <rest.h>

#include <cctype>
#include <utility>

namespace sc::lxd {
    namespace {
        // A path segment or query value, %XX-encoded but for unreserved characters.
        std::string encoded(const std::string_view text) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string out;
            for (const char c: text) {
                const auto byte = static_cast<unsigned char>(c);
                if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~') {
                    out += c;
                } else {
                    out += '%';
                    out += hex[byte >> 4];
                    out += hex[byte & 15];
                }
            }
            return out;
        }

        std::optional<std::int64_t> number_at(const nlohmann::json &object, const std::initializer_list<const char *> path) {
            const nlohmann::json *at = &object;
            for (const char *key: path) {
                if (!at->is_object() || !at->contains(key)) return std::nullopt;
                at = &(*at)[key];
            }
            if (!at->is_number()) return std::nullopt;
            return at->get<std::int64_t>();
        }

        std::string text_at(const nlohmann::json &object, const char *key) {
            const auto found = object.find(key);
            return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
        }
    }

    instance instance::from_json(const nlohmann::json &metadata) {
        instance out;
        out.name = text_at(metadata, "name");
        out.project = text_at(metadata, "project");
        out.location = text_at(metadata, "location");
        out.type = text_at(metadata, "type");
        out.description = text_at(metadata, "description");
        out.status = text_at(metadata, "status");
        out.status_code = metadata.value("status_code", 0);
        out.created_at = text_at(metadata, "created_at");
        out.last_used_at = text_at(metadata, "last_used_at");
        const auto state = metadata.find("state");
        if (state == metadata.end() || !state->is_object()) return out;
        if (state->contains("status_code") && (*state)["status_code"].is_number()) out.status_code = (*state)["status_code"].get<int>();
        if (const auto status = text_at(*state, "status"); !status.empty()) out.status = status;
        // A stopped instance reports 0 bytes of memory: that's none in use, not a reading.
        if (const auto memory = number_at(*state, {"memory", "usage"}); memory && *memory > 0) out.memory_usage = memory;
        out.disk_usage = number_at(*state, {"disk", "root", "usage"});
        if (const auto network = state->find("network"); network != state->end() && network->is_object()) {
            for (const auto &[interface, details]: network->items()) {
                if (interface == "lo" || !details.contains("addresses") || !details["addresses"].is_array()) continue;
                for (const auto &address: details["addresses"])
                    if (text_at(address, "family") == "inet" && text_at(address, "scope") == "global")
                        out.ipv4.push_back(text_at(address, "address"));
            }
        }
        return out;
    }

    client::client(std::vector<std::string> members, credentials how, const std::chrono::seconds timeout)
        : members_(std::move(members)), credentials_(std::move(how)), timeout_(timeout) {
        if (members_.empty()) throw std::invalid_argument("sc::lxd::client: no member URLs");
        for (auto &member: members_)
            while (member.ends_with('/')) member.pop_back();
        if (credentials_.certificate.empty() || credentials_.key.empty())
            throw std::invalid_argument("sc::lxd::client: a client certificate and its key are needed");
    }

    nlohmann::json client::get(const std::string_view path) const {
        std::size_t first;
        {
            const std::lock_guard lock{mutex_};
            first = answered_;
        }
        std::string reasons;
        for (std::size_t tried = 0; tried < members_.size(); ++tried) {
            const std::size_t index = (first + tried) % members_.size();
            sc::rest request{members_[index] + std::string{path}};
            request.quiet().client_certificate(credentials_.certificate.string(), credentials_.key.string());
            if (!credentials_.server_certificate.empty()) request.trust(credentials_.server_certificate.string()).verify_host(false);
            if (!credentials_.pinned_key.empty()) request.pinned_public_key(credentials_.pinned_key);
            request.connect_timeout(std::min<long>(5, static_cast<long>(timeout_.count())));
            request.timeout(static_cast<long>(timeout_.count()));
            const auto body = request.get();
            const long status = request.status();
            std::string why;
            if (status == 0) {
                why = request.error();
            } else if (status == 503) {
                why = "503 (not ready)";
            } else {
                const auto answer = nlohmann::json::parse(body, nullptr, false);
                if (answer.is_object() && answer.contains("type")) {
                    {
                        const std::lock_guard lock{mutex_};
                        answered_ = index;
                    }
                    if (answer["type"] == "error")
                        throw lxd::error{text_at(answer, "error"), answer.value("error_code", static_cast<int>(status))};
                    return answer.value("metadata", nlohmann::json{});
                }
                why = "HTTP " + std::to_string(status) + ", not LXD's answer";
            }
            if (!reasons.empty()) reasons += "; ";
            reasons += members_[index] + ": " + why;
        }
        throw unreachable{"sc::lxd: no member answered GET " + std::string{path} + " (" + reasons + ")"};
    }

    std::vector<instance> client::instances(const bool all_projects) const {
        const auto listed = get(all_projects ? "/1.0/instances?recursion=2&all-projects=true" : "/1.0/instances?recursion=2");
        std::vector<instance> out;
        if (!listed.is_array()) return out;
        out.reserve(listed.size());
        for (const auto &each: listed) out.push_back(instance::from_json(each));
        return out;
    }

    std::optional<instance> client::find(const std::string_view name, const std::string_view project) const {
        try {
            // recursion=1 on one instance brings its state along (InstanceFull).
            return instance::from_json(get("/1.0/instances/" + encoded(name) + "?recursion=1&project=" + encoded(project)));
        } catch (const lxd::error &refused) {
            if (refused.status() == 404) return std::nullopt;
            throw;
        }
    }

    std::string client::member() const {
        const std::lock_guard lock{mutex_};
        return members_[answered_];
    }
}
