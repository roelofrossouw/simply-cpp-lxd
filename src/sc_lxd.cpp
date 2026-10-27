#include "sc_lxd.h"

#include <rest.h>

#include <cctype>
#include <cstdio>
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

    client::response client::request(const char *method, const std::string_view path, const std::string *body, const bool raw,
                                     const std::chrono::seconds timeout, const std::map<std::string, std::string> *headers) const {
        std::size_t first;
        {
            const std::lock_guard lock{mutex_};
            first = answered_;
        }
        const auto limit = timeout.count() > 0 ? timeout : timeout_;
        std::string reasons;
        for (std::size_t tried = 0; tried < members_.size(); ++tried) {
            const std::size_t index = (first + tried) % members_.size();
            sc::rest call{members_[index] + std::string{path}};
            call.quiet().client_certificate(credentials_.certificate.string(), credentials_.key.string());
            if (!credentials_.server_certificate.empty()) call.trust(credentials_.server_certificate.string()).verify_host(false);
            if (!credentials_.pinned_key.empty()) call.pinned_public_key(credentials_.pinned_key);
            call.connect_timeout(std::min<long>(5, static_cast<long>(timeout_.count())));
            call.timeout(static_cast<long>(limit.count()));
            if (headers)
                for (const auto &[name, value]: *headers) call.header(name, value);
            const std::string_view verb{method};
            auto answer_body = verb == "POST"     ? call.post(body ? *body : std::string{"{}"})
                               : verb == "PUT"    ? call.put(body ? *body : std::string{"{}"})
                               : verb == "DELETE" ? call.del()
                                                  : call.get();
            const long status = call.status();
            std::string why;
            if (status == 0) {
                why = call.error();
            } else if (status == 503) {
                why = "503 (not ready)";
            } else {
                // LXD's own answer: its JSON, or (raw) a file's bytes, which come with X-LXD-type.
                const auto parsed = nlohmann::json::parse(answer_body, nullptr, false);
                const bool lxds = (parsed.is_object() && parsed.contains("type")) ||
                                  (raw && (status == 200 || call.response_headers().contains("x-lxd-type")));
                if (lxds) {
                    {
                        const std::lock_guard lock{mutex_};
                        answered_ = index;
                    }
                    return {status, std::move(answer_body), call.response_headers()};
                }
                why = "HTTP " + std::to_string(status) + ", not LXD's answer";
            }
            if (!reasons.empty()) reasons += "; ";
            reasons += members_[index] + ": " + why;
        }
        throw unreachable{"sc::lxd: no member answered " + std::string{method} + " " + std::string{path} + " (" + reasons + ")"};
    }

    nlohmann::json client::metadata_of(const response &answer) {
        const auto parsed = nlohmann::json::parse(answer.body, nullptr, false);
        if (!parsed.is_object()) throw lxd::error{"sc::lxd: an answer that isn't JSON", static_cast<int>(answer.status)};
        if (parsed.value("type", "") == "error")
            throw lxd::error{text_at(parsed, "error"), parsed.value("error_code", static_cast<int>(answer.status))};
        return parsed.value("metadata", nlohmann::json{});
    }

    nlohmann::json client::get(const std::string_view path) const { return metadata_of(request("GET", path)); }

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

    std::optional<file> client::read_file(const std::string_view instance, const std::string_view path, const std::string_view project) const {
        const auto answer = request("GET", "/1.0/instances/" + encoded(instance) + "/files?path=" + encoded(path) + "&project=" + encoded(project),
                                    nullptr, true);
        const auto header = [&](const char *name) {
            const auto found = answer.headers.find(name);
            return found == answer.headers.end() ? std::string{} : found->second;
        };
        const auto type = header("x-lxd-type");
        if (type.empty()) {
            try {
                (void) metadata_of(answer);
            } catch (const lxd::error &refused) {
                if (refused.status() == 404) return std::nullopt;
                throw;
            }
            throw lxd::error{"sc::lxd: no file type in the answer for " + std::string{path}, static_cast<int>(answer.status)};
        }
        file out;
        out.type = type;
        const auto number = [&](const char *name, const int base) {
            const auto text = header(name);
            try {
                return text.empty() ? 0 : std::stoi(text, nullptr, base);
            } catch (const std::exception &) {
                return 0;
            }
        };
        out.uid = number("x-lxd-uid", 10);
        out.gid = number("x-lxd-gid", 10);
        out.mode = static_cast<unsigned>(number("x-lxd-mode", 8));
        if (type == "directory") {
            if (const auto names = metadata_of(answer); names.is_array())
                for (const auto &name: names)
                    if (name.is_string()) out.entries.push_back(name.get<std::string>());
        } else {
            out.content = answer.body;
        }
        return out;
    }

    void client::write_file(const std::string_view instance, const std::string_view path, const file &what, const std::string_view project) const {
        if (what.type != "file" && what.type != "symlink" && what.type != "directory")
            throw std::invalid_argument("sc::lxd::client::write_file: a type of file, symlink or directory, not \"" + what.type + "\"");
        char mode[8];
        std::snprintf(mode, sizeof mode, "%04o", what.mode & 07777);
        const std::map<std::string, std::string> headers{{"Content-Type", "application/octet-stream"}, {"X-LXD-type", what.type},
                                                         {"X-LXD-uid", std::to_string(what.uid)}, {"X-LXD-gid", std::to_string(what.gid)},
                                                         {"X-LXD-mode", mode}, {"X-LXD-write", "overwrite"},
                                                         // Else LXD keeps what's there's owner and mode (instances_files_modify_permissions).
                                                         {"X-LXD-modify-perm", "uid,gid,mode"}};
        const std::string body = what.type == "directory" ? std::string{} : what.content;
        (void) metadata_of(request("POST", "/1.0/instances/" + encoded(instance) + "/files?path=" + encoded(path) + "&project=" + encoded(project),
                                   &body, false, std::chrono::seconds{0}, &headers));
    }

    bool client::delete_file(const std::string_view instance, const std::string_view path, const std::string_view project) const {
        try {
            (void) metadata_of(request("DELETE", "/1.0/instances/" + encoded(instance) + "/files?path=" + encoded(path) + "&project=" + encoded(project)));
            return true;
        } catch (const lxd::error &refused) {
            if (refused.status() == 404) return false;
            throw;
        }
    }

    exec_result client::exec(const std::string_view instance, const std::vector<std::string> &command, const exec_options &options) const {
        if (command.empty()) throw std::invalid_argument("sc::lxd::client::exec: no command");
        nlohmann::json order{{"command", command}, {"wait-for-websocket", false}, {"interactive", false}, {"record-output", true}};
        order["environment"] = nlohmann::json::object();
        for (const auto &[name, value]: options.environment) order["environment"][name] = value;
        const auto body = order.dump();
        const auto started = metadata_of(request("POST", "/1.0/instances/" + encoded(instance) + "/exec?project=" + encoded(options.project), &body));
        const auto id = text_at(started, "id");
        if (id.empty()) throw lxd::error{"sc::lxd: exec started no operation", 0};

        // The wait answers when it ends, or with it still running at the limit.
        const auto seconds = std::to_string(options.limit.count());
        const auto ended = metadata_of(request("GET", "/1.0/operations/" + encoded(id) + "/wait?timeout=" + seconds + "&project=" + encoded(options.project),
                                               nullptr, false, options.limit + timeout_));
        const auto status = text_at(ended, "status");
        if (status == "Running") throw lxd::error{"sc::lxd: " + std::string{instance} + ": the command was still running after " + seconds + " s", 408};
        if (status != "Success") throw lxd::error{"sc::lxd: " + std::string{instance} + ": " + text_at(ended, "err"), 500};

        exec_result out;
        const auto details = ended.value("metadata", nlohmann::json::object());
        out.status = details.value("return", -1);
        const auto output = details.value("output", nlohmann::json::object());
        const auto collect = [&](const char *stream, std::string &into) {
            const auto log = text_at(output, stream);
            if (log.empty()) return;
            into = request("GET", log, nullptr, true).body;
            try {
                (void) metadata_of(request("DELETE", log));
            } catch (const std::exception &) {
                // A log left behind is LXD's to clear (at the instance's next start); the result stands.
            }
        };
        collect("1", out.out);
        collect("2", out.err);
        return out;
    }

    void client::wait_for(const nlohmann::json &operation, const std::string_view project, const std::chrono::seconds limit,
                          const std::string &what) const {
        const auto id = text_at(operation, "id");
        if (id.empty()) throw lxd::error{"sc::lxd: " + what + " started no operation", 0};
        // The wait answers when it ends, or with it still running at the limit.
        const auto seconds = std::to_string(limit.count());
        const auto ended = metadata_of(request("GET", "/1.0/operations/" + encoded(id) + "/wait?timeout=" + seconds + "&project=" + encoded(project),
                                               nullptr, false, limit + timeout_));
        const auto status = text_at(ended, "status");
        if (status == "Running") throw lxd::error{"sc::lxd: " + what + " was still going after " + seconds + " s", 408};
        if (status != "Success") throw lxd::error{"sc::lxd: " + what + ": " + text_at(ended, "err"), 500};
    }

    void client::create(const nlohmann::json &body, const std::string_view project, const std::string_view target, const std::chrono::seconds limit) const {
        const auto text = body.dump();
        const auto name = body.value("name", std::string{});
        const auto started = metadata_of(request("POST", "/1.0/instances?project=" + encoded(project) +
                                                             (target.empty() ? std::string{} : "&target=" + encoded(target)), &text));
        wait_for(started, project, limit, "making " + name);
    }

    void client::change_state(const std::string_view instance, const std::string_view action, const std::string_view project, const bool force,
                              const std::chrono::seconds limit) const {
        const auto text = nlohmann::json{{"action", action}, {"timeout", static_cast<int>(limit.count())}, {"force", force}}.dump();
        const auto started = metadata_of(request("PUT", "/1.0/instances/" + encoded(instance) + "/state?project=" + encoded(project), &text));
        wait_for(started, project, limit, std::string{action} + " " + std::string{instance});
    }

    void client::set_config(const std::string_view instance, const std::map<std::string, std::string> &values, const std::string_view project,
                            const std::chrono::seconds limit) const {
        const auto path = "/1.0/instances/" + encoded(instance) + "?project=" + encoded(project);
        for (int attempt = 1;; ++attempt) {
            const auto read = request("GET", path);
            const auto current = metadata_of(read);
            // What PUT takes (InstancePut), as it is now.
            nlohmann::json body = nlohmann::json::object();
            for (const char *key: {"architecture", "config", "devices", "ephemeral", "profiles", "stateful", "description"})
                if (current.contains(key)) body[key] = current[key];
            if (!body.contains("config") || !body["config"].is_object()) body["config"] = nlohmann::json::object();
            for (const auto &[key, value]: values) body["config"][key] = value;
            std::map<std::string, std::string> headers;
            if (const auto tag = read.headers.find("etag"); tag != read.headers.end()) headers["If-Match"] = tag->second;
            const auto text = body.dump();
            try {
                const auto answer = request("PUT", path, &text, false, std::chrono::seconds{0}, &headers);
                const auto started = metadata_of(answer);
                if (nlohmann::json::parse(answer.body, nullptr, false).value("type", "") == "async")
                    wait_for(started, project, limit, "changing " + std::string{instance} + "'s config");
                return;
            } catch (const lxd::error &refused) {
                if (refused.status() != 412 || attempt == 3) throw; // 412: changed since it was read
            }
        }
    }

    bool client::remove(const std::string_view instance, const std::string_view project, const std::chrono::seconds limit) const {
        const auto there = find(instance, project);
        if (!there) return false;
        if (there->status_code != 102) change_state(instance, "stop", project, true, limit); // 102: Stopped
        const auto started = metadata_of(request("DELETE", "/1.0/instances/" + encoded(instance) + "?project=" + encoded(project)));
        wait_for(started, project, limit, "removing " + std::string{instance});
        return true;
    }

    std::string client::member() const {
        const std::lock_guard lock{mutex_};
        return members_[answered_];
    }
}
