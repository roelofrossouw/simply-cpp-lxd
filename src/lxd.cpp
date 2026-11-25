#include <sc/lxd/lxd.h>

#include <sc/http/client.h>

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <future>
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

        // %XX decoded (a name LXD gave in a URL).
        std::string decoded(const std::string_view text) {
            std::string out;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                    out += static_cast<char>(std::stoi(std::string{text.substr(i + 1, 2)}, nullptr, 16));
                    i += 2;
                } else {
                    out += text[i];
                }
            }
            return out;
        }

        constexpr std::string_view instances_prefix = "/1.0/instances";

        // A query parameter's value ("project" in "a=1&project=x"), decoded; "" when it isn't there.
        std::string parameter(const std::string_view query, const std::string_view name) {
            std::size_t at = 0;
            while (at <= query.size()) {
                const auto end = std::min(query.find('&', at), query.size());
                const auto pair = query.substr(at, end - at);
                if (pair.size() > name.size() && pair.substr(0, name.size()) == name && pair[name.size()] == '=')
                    return decoded(pair.substr(name.size() + 1));
                at = end + 1;
            }
            return {};
        }

        // An instance's name and project from a path or URL LXD gives ("/1.0/instances/web1?project=x",
        // "/1.0/instances/web1/files?path=..&project=x"); nothing for any other path.
        std::optional<std::pair<std::string, std::string>> instance_of(const std::string_view path) {
            const std::string_view prefix = "/1.0/instances/";
            if (!path.starts_with(prefix)) return std::nullopt;
            const auto rest = path.substr(prefix.size());
            const auto name_end = std::min(rest.find_first_of("/?"), rest.size());
            if (name_end == 0) return std::nullopt;
            const auto query_at = rest.find('?');
            auto project = query_at == std::string_view::npos ? std::string{} : parameter(rest.substr(query_at + 1), "project");
            if (project.empty()) project = "default";
            return std::pair{decoded(rest.substr(0, name_end)), project};
        }

        // The instances' list: /1.0/instances or /1.0/instances?...
        bool instances_list(const std::string_view path) {
            return path == instances_prefix || (path.starts_with(instances_prefix) && path.size() > instances_prefix.size() &&
                                                path[instances_prefix.size()] == '?');
        }

        std::string key_of(const std::string_view project, const std::string_view name) {
            return std::string{project.empty() ? std::string_view{"default"} : project} + "/" + std::string{name};
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
        // The rest only mean something while it runs: a stopped one reports zeros (processes -1).
        if (out.status_code == 103) {
            out.swap_usage = number_at(*state, {"memory", "swap_usage"});
            out.cpu_usage = number_at(*state, {"cpu", "usage"});
            if (const auto running = number_at(*state, {"processes"}); running && *running >= 0) out.processes = running;
        }
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

    client::client(std::vector<std::string> members, credentials how, std::vector<standalone> hosts, const std::chrono::seconds timeout)
        : client(std::move(members), std::move(how), timeout) {
        std::set<std::string> names;
        for (auto &host: hosts) {
            if (host.name.empty()) throw std::invalid_argument("sc::lxd::client: a standalone host without a name");
            if (!names.insert(host.name).second) throw std::invalid_argument("sc::lxd::client: standalone host " + host.name + " twice");
            if (host.urls.empty()) throw std::invalid_argument("sc::lxd::client: standalone host " + host.name + " has no URL");
            standalone_.push_back({host.name, std::make_unique<client>(std::move(host.urls), std::move(host.how), timeout), {}, {}});
        }
    }

    std::vector<std::string> client::hosts() const {
        std::vector<std::string> out;
        for (const auto &each: standalone_) out.push_back(each.name);
        return out;
    }

    const client *client::standalone_named(const std::string_view host) const {
        for (const auto &each: standalone_)
            if (each.name == host) return each.lxd.get();
        return nullptr;
    }

    void client::place(const std::string_view project, const std::string_view name, const std::string_view host) const {
        if (standalone_.empty()) return;
        const std::lock_guard lock{route_mutex_};
        if (standalone_named(host)) pinned_[key_of(project, name)] = std::string{host};
        else pinned_.erase(key_of(project, name));
    }

    void client::refresh_listings() const {
        const auto now = std::chrono::steady_clock::now();
        for (auto &each: standalone_) {
            {
                const std::lock_guard lock{route_mutex_};
                if (now < each.next_listing) continue;
                // Taken now, so another call doesn't read it too meanwhile.
                each.next_listing = now + std::chrono::seconds{30};
            }
            std::set<std::string> seen;
            bool answered = false;
            try {
                const auto listed = each.lxd->get("/1.0/instances?all-projects=true");
                if (listed.is_array())
                    for (const auto &url: listed)
                        if (url.is_string())
                            if (const auto found = instance_of(url.get<std::string>())) seen.insert(key_of(found->second, found->first));
                answered = true;
            } catch (const std::exception &) {
                // Its last listing stands; it's tried again in 5 minutes, not every call meanwhile.
            }
            const std::lock_guard lock{route_mutex_};
            if (answered) each.seen = std::move(seen);
            else each.next_listing = now + std::chrono::minutes{5};
        }
    }

    const client *client::route(const std::string_view name, const std::string_view project) const {
        if (standalone_.empty()) return nullptr;
        const auto key = key_of(project, name);
        {
            const std::lock_guard lock{route_mutex_};
            if (const auto pin = pinned_.find(key); pin != pinned_.end()) return standalone_named(pin->second);
        }
        refresh_listings();
        const std::lock_guard lock{route_mutex_};
        for (const auto &each: standalone_)
            if (each.seen.contains(key)) return each.lxd.get();
        return nullptr;
    }

    std::string client::host_of(const std::string_view name, const std::string_view project) const {
        const auto *found = route(name, project);
        for (const auto &each: standalone_)
            if (each.lxd.get() == found) return each.name;
        return {};
    }

    std::set<std::string> client::on_host(const std::string_view host) const {
        std::set<std::string> out;
        const std::lock_guard lock{route_mutex_};
        for (const auto &[key, pinned_host]: pinned_)
            if (pinned_host == host) out.insert(key);
        for (const auto &each: standalone_)
            if (each.name == host) out.insert(each.seen.begin(), each.seen.end());
        return out;
    }

    nlohmann::json client::get_on(const std::string_view host, const std::string_view path) const {
        if (const auto *other = standalone_named(host)) return other->get(path);
        return metadata_of(request("GET", path));
    }

    listing client::list(const bool all_projects) const {
        listing out;
        std::vector<instance> cluster;
        if (read_members()) {
            cluster = cluster_instances(all_projects, out.members);
        } else {
            const auto listed = metadata_of(request("GET", all_projects ? "/1.0/instances?recursion=2&all-projects=true" : "/1.0/instances?recursion=2"));
            if (listed.is_array())
                for (const auto &each: listed) cluster.push_back(instance::from_json(each));
        }
        std::set<std::string> elsewhere;
        for (auto &each: standalone_) {
            std::vector<instance> theirs;
            try {
                theirs = each.lxd->instances(all_projects);
            } catch (const std::exception &) {
                out.unanswered.push_back(each.name);
                continue;
            }
            std::set<std::string> seen;
            for (auto &one: theirs) {
                one.location = each.name; // LXD says "none" outside a cluster
                if (one.project.empty()) one.project = "default";
                seen.insert(key_of(one.project, one.name));
            }
            {
                const std::lock_guard lock{route_mutex_};
                // A listing of the default project only doesn't say what the others have.
                if (all_projects) each.seen = seen;
                else each.seen.insert(seen.begin(), seen.end());
                each.next_listing = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            }
            elsewhere.insert(seen.begin(), seen.end());
            for (auto &one: theirs) out.instances.push_back(std::move(one));
        }
        for (auto &one: cluster)
            if (!elsewhere.contains(key_of(one.project, one.name))) out.instances.push_back(std::move(one));
        return out;
    }

    client::attempt client::try_on(const std::string &url, const char *method, const std::string_view path, const std::string *body, const bool raw,
                                   const std::chrono::seconds limit, const std::map<std::string, std::string> *headers) const {
        attempt out;
        sc::http::client call{url + std::string{path}};
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
        if (status == 0) {
            out.why = call.error();
            // curl's 28 after connecting ("Operation timed out after ..."; not "Connection timed out" or
            // "Resolving timed out"): the member took the call and is still at it.
            out.read_timeout = call.error_code() == 28 && out.why.find("Operation timed out") != std::string::npos;
            return out;
        }
        if (status == 503) {
            out.why = "503 (not ready)";
            return out;
        }
        // LXD's own answer: its JSON, or (raw) a file's bytes, which come with X-LXD-type.
        const auto parsed = nlohmann::json::parse(answer_body, nullptr, false);
        const bool lxds = (parsed.is_object() && parsed.contains("type")) || (raw && (status == 200 || call.response_headers().contains("x-lxd-type")));
        if (!lxds) {
            out.why = "HTTP " + std::to_string(status) + ", not LXD's answer";
            return out;
        }
        // An operation started here is waited for here.
        if (parsed.is_object())
            if (const auto operation = text_at(parsed, "operation"); operation.starts_with("/1.0/operations/")) {
                auto id = operation.substr(std::string_view{"/1.0/operations/"}.size());
                id = id.substr(0, id.find_first_of("/?"));
                const std::lock_guard lock{placement_mutex_};
                if (operations_.size() > 1000) operations_.clear();
                operations_[id] = url;
            }
        out.answer = response{status, std::move(answer_body), call.response_headers()};
        return out;
    }

    client::response client::request(const char *method, const std::string_view path, const std::string *body, const bool raw,
                                     const std::chrono::seconds timeout, const std::map<std::string, std::string> *headers) const {
        std::size_t first;
        {
            const std::lock_guard lock{mutex_};
            first = answered_;
        }
        const auto limit = timeout.count() > 0 ? timeout : timeout_;
        const auto prefer = preferred(path);
        std::vector<std::pair<std::string, std::optional<std::size_t>>> order; // URL, members_' index (none: the preferred one)
        if (!prefer.empty()) order.emplace_back(prefer, std::nullopt);
        for (std::size_t tried = 0; tried < members_.size(); ++tried) {
            const std::size_t index = (first + tried) % members_.size();
            if (members_[index] != prefer) order.emplace_back(members_[index], index);
        }
        std::string reasons;
        for (const auto &[url, index]: order) {
            auto got = try_on(url, method, path, body, raw, limit, headers);
            if (got.answer) {
                if (index) {
                    const std::lock_guard lock{mutex_};
                    answered_ = *index;
                }
                return std::move(*got.answer);
            }
            if (!reasons.empty()) reasons += "; ";
            reasons += url + ": " + got.why;
            // Asking another member would only have it forward the same call to the same busy one.
            if (got.read_timeout)
                throw unreachable{"sc::lxd: " + std::string{method} + " " + std::string{path} + " timed out after " + std::to_string(limit.count()) +
                                  " s (" + reasons + "; not asked of the other members)"};
        }
        throw unreachable{"sc::lxd: no member answered " + std::string{method} + " " + std::string{path} + " (" + reasons + ")"};
    }

    bool client::read_members() const {
        const auto now = std::chrono::steady_clock::now();
        {
            const std::lock_guard lock{placement_mutex_};
            if (now < members_next_) return clustered_;
            members_next_ = now + std::chrono::minutes{10}; // taken now, so others don't read it meanwhile
        }
        std::map<std::string, std::string> urls, status;
        bool clustered = false;
        try {
            const auto listed = metadata_of(request("GET", "/1.0/cluster/members?recursion=1"));
            if (listed.is_array())
                for (const auto &each: listed) {
                    auto url = text_at(each, "url");
                    while (url.ends_with('/')) url.pop_back();
                    const auto name = text_at(each, "server_name");
                    if (name.empty() || !url.starts_with("https://")) continue;
                    urls[name] = url;
                    status[name] = text_at(each, "status");
                }
            clustered = !urls.empty();
        } catch (const lxd::error &) {
            clustered = false; // LXD's no: not a cluster
        } catch (const std::exception &) {
            const std::lock_guard lock{placement_mutex_};
            members_next_ = now + std::chrono::seconds{30}; // not answered: tried again soon, what was known kept
            return clustered_;
        }
        const std::lock_guard lock{placement_mutex_};
        clustered_ = clustered;
        member_urls_ = std::move(urls);
        member_status_ = std::move(status);
        return clustered_;
    }

    void client::read_locations(const bool all_projects, const bool force) const {
        const auto now = std::chrono::steady_clock::now();
        {
            const std::lock_guard lock{placement_mutex_};
            if (!force && now < locations_next_) return;
            locations_next_ = now + std::chrono::minutes{5};
        }
        const auto listed = metadata_of(request("GET", all_projects ? "/1.0/instances?recursion=1&all-projects=true" : "/1.0/instances?recursion=1"));
        if (!listed.is_array()) return;
        std::map<std::string, located> found;
        for (const auto &each: listed) {
            auto project = text_at(each, "project");
            if (project.empty()) project = "default";
            found[key_of(project, text_at(each, "name"))] = {text_at(each, "location"), each};
        }
        const std::lock_guard lock{placement_mutex_};
        if (all_projects) located_ = std::move(found);
        else
            for (auto &[key, where]: found) located_[key] = std::move(where);
    }

    std::string client::preferred(const std::string_view path) const {
        if (path.starts_with("/1.0/operations/")) {
            auto id = std::string{path.substr(std::string_view{"/1.0/operations/"}.size())};
            id = id.substr(0, id.find_first_of("/?"));
            const std::lock_guard lock{placement_mutex_};
            const auto found = operations_.find(id);
            return found == operations_.end() ? std::string{} : found->second;
        }
        const auto instance = instance_of(path);
        if (!instance) return {};
        if (!read_members()) return {};
        try {
            read_locations(true, false); // at most every 5 minutes
        } catch (const std::exception &) {
            // not known: the member that answered last takes it, as before
        }
        const std::lock_guard lock{placement_mutex_};
        const auto where = located_.find(key_of(instance->second, instance->first));
        if (where == located_.end()) return {};
        const auto url = member_urls_.find(where->second.member);
        return url == member_urls_.end() ? std::string{} : url->second;
    }

    std::vector<instance> client::cluster_instances(const bool all_projects, std::vector<member_health> &health) const {
        // The names (from the database: no member forwards this), then where each is when one's new.
        const auto names = metadata_of(request("GET", all_projects ? "/1.0/instances?all-projects=true" : "/1.0/instances"));
        std::vector<std::string> keys;
        if (names.is_array())
            for (const auto &url: names)
                if (url.is_string())
                    if (const auto found = instance_of(url.get<std::string>())) keys.push_back(key_of(found->second, found->first));
        bool unknown = false;
        {
            const std::lock_guard lock{placement_mutex_};
            unknown = std::ranges::any_of(keys, [&](const std::string &key) { return !located_.contains(key); });
        }
        read_locations(all_projects, unknown);

        // By member: GET /1.0, then its instances' state, 4 at a time, the members side by side.
        std::map<std::string, std::vector<std::pair<std::string, nlohmann::json>>> by_member; // member -> (key, listed)
        std::map<std::string, std::string> urls, status;
        {
            const std::lock_guard lock{placement_mutex_};
            for (const auto &key: keys)
                if (const auto where = located_.find(key); where != located_.end()) by_member[where->second.member].emplace_back(key, where->second.listed);
            urls = member_urls_;
            status = member_status_;
        }
        const auto limit = std::min(timeout_, std::chrono::seconds{10});
        struct read {
            member_health health;
            std::vector<std::pair<std::string, nlohmann::json>> instances; // listed, with "state" when it was read
        };
        std::vector<std::future<read>> reads;
        for (auto &entry: by_member) {
            reads.push_back(std::async(std::launch::async, [&urls, &status, &limit, this, member = entry.first, listed = std::move(entry.second)]() mutable {
                read out;
                out.health.name = member;
                const auto url = urls.find(member);
                const auto online = status.find(member);
                if (url == urls.end() || (online != status.end() && online->second != "Online")) {
                    out.health.error = url == urls.end() ? "no URL for it" : "it's " + online->second;
                    out.health.states_skipped = static_cast<int>(listed.size());
                    out.instances = std::move(listed);
                    return out;
                }
                out.health.url = url->second;
                const auto member_url = url->second;
                // One GET on this member: its metadata, how long it took, why not.
                struct got_one {
                    std::optional<nlohmann::json> metadata;
                    double ms = -1;
                    std::string why;
                };
                const auto timed = [this, member_url, limit](const std::string &path) {
                    got_one one;
                    const auto started = std::chrono::steady_clock::now();
                    auto got = try_on(member_url, "GET", path, nullptr, false, limit, nullptr);
                    one.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    if (!got.answer) {
                        one.why = path + ": " + got.why;
                        return one;
                    }
                    try {
                        one.metadata = metadata_of(*got.answer);
                    } catch (const std::exception &refused) {
                        one.why = path + ": " + refused.what();
                    }
                    return one;
                };
                const auto api = timed("/1.0");
                if (api.metadata) out.health.api_ms = api.ms;
                else out.health.error = api.why;
                bool stop = !out.health.error.empty();
                for (std::size_t at = 0; at < listed.size(); at += 4) {
                    const auto end = std::min(listed.size(), at + 4);
                    if (stop) {
                        out.health.states_skipped += static_cast<int>(end - at);
                        continue;
                    }
                    std::vector<std::future<got_one>> batch;
                    for (auto i = at; i < end; ++i) {
                        const auto &each = listed[i].second;
                        auto project = text_at(each, "project");
                        if (project.empty()) project = "default";
                        const auto path = "/1.0/instances/" + encoded(text_at(each, "name")) + "/state?project=" + encoded(project);
                        batch.push_back(std::async(std::launch::async, timed, path));
                    }
                    for (auto i = at; i < end; ++i) {
                        auto one = batch[i - at].get();
                        if (one.metadata && one.metadata->is_object()) {
                            listed[i].second["state"] = std::move(*one.metadata);
                            ++out.health.states_read;
                            out.health.state_max_ms = std::max(out.health.state_max_ms, one.ms);
                        } else {
                            ++out.health.states_skipped;
                            if (out.health.error.empty()) out.health.error = one.why.empty() ? "no state in its answer" : one.why;
                            stop = true; // the rest of this member's left for the next listing
                        }
                    }
                }
                out.instances = std::move(listed);
                return out;
            }));
        }
        std::map<std::string, instance> made;
        for (auto &future: reads) {
            auto got = future.get();
            for (auto &[key, listed]: got.instances) {
                auto one = instance::from_json(listed);
                one.state_read = listed.contains("state") && listed["state"].is_object();
                if (one.project.empty()) one.project = "default";
                made[key] = std::move(one);
            }
            health.push_back(std::move(got.health));
        }
        // In the names' order; one listed but not yet located (made meanwhile) comes as the names say.
        std::vector<instance> out;
        out.reserve(keys.size());
        for (const auto &key: keys)
            if (auto found = made.find(key); found != made.end()) out.push_back(std::move(found->second));
        return out;
    }

    nlohmann::json client::metadata_of(const response &answer) {
        const auto parsed = nlohmann::json::parse(answer.body, nullptr, false);
        if (!parsed.is_object()) throw lxd::error{"sc::lxd: an answer that isn't JSON", static_cast<int>(answer.status)};
        if (parsed.value("type", "") == "error")
            throw lxd::error{text_at(parsed, "error"), parsed.value("error_code", static_cast<int>(answer.status))};
        return parsed.value("metadata", nlohmann::json{});
    }

    nlohmann::json client::get(const std::string_view path) const {
        if (!standalone_.empty()) {
            if (const auto found = instance_of(path)) {
                if (const auto *other = route(found->first, found->second)) {
                    auto out = other->get(path);
                    if (out.is_object() && out.contains("location"))
                        for (const auto &each: standalone_)
                            if (each.lxd.get() == other) out["location"] = each.name;
                    return out;
                }
            } else if (instances_list(path)) {
                auto out = metadata_of(request("GET", path));
                if (!out.is_array()) return out;
                std::set<std::string> elsewhere;
                nlohmann::json theirs = nlohmann::json::array();
                for (const auto &each: standalone_) {
                    nlohmann::json listed;
                    try {
                        listed = each.lxd->get(path);
                    } catch (const std::exception &) {
                        continue; // a standalone host that doesn't answer: the others' list stands
                    }
                    if (!listed.is_array()) continue;
                    for (auto &one: listed) {
                        if (one.is_object()) {
                            one["location"] = each.name;
                            elsewhere.insert(key_of(text_at(one, "project"), text_at(one, "name")));
                        } else if (one.is_string()) {
                            if (const auto found = instance_of(one.get<std::string>())) elsewhere.insert(key_of(found->second, found->first));
                        }
                        theirs.push_back(std::move(one));
                    }
                }
                nlohmann::json merged = nlohmann::json::array();
                for (auto &one: out) {
                    std::string key;
                    if (one.is_object()) key = key_of(text_at(one, "project"), text_at(one, "name"));
                    else if (one.is_string())
                        if (const auto found = instance_of(one.get<std::string>())) key = key_of(found->second, found->first);
                    if (key.empty() || !elsewhere.contains(key)) merged.push_back(std::move(one));
                }
                for (auto &one: theirs) merged.push_back(std::move(one));
                return merged;
            }
        }
        return metadata_of(request("GET", path));
    }

    std::vector<instance> client::instances(const bool all_projects) const { return list(all_projects).instances; }

    std::optional<instance> client::find(const std::string_view name, const std::string_view project) const {
        if (const auto *other = route(name, project)) {
            auto found = other->find(name, project);
            if (found) {
                for (const auto &each: standalone_)
                    if (each.lxd.get() == other) found->location = each.name;
            }
            return found;
        }
        try {
            // recursion=1 on one instance brings its state along (InstanceFull).
            return instance::from_json(get("/1.0/instances/" + encoded(name) + "?recursion=1&project=" + encoded(project)));
        } catch (const lxd::error &refused) {
            if (refused.status() == 404) return std::nullopt;
            throw;
        }
    }

    std::optional<file> client::read_file(const std::string_view instance, const std::string_view path, const std::string_view project) const {
        if (const auto *other = route(instance, project)) return other->read_file(instance, path, project);
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
        if (const auto *other = route(instance, project)) return other->write_file(instance, path, what, project);
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
        if (const auto *other = route(instance, project)) return other->delete_file(instance, path, project);
        try {
            (void) metadata_of(request("DELETE", "/1.0/instances/" + encoded(instance) + "/files?path=" + encoded(path) + "&project=" + encoded(project)));
            return true;
        } catch (const lxd::error &refused) {
            if (refused.status() == 404) return false;
            throw;
        }
    }

    exec_result client::exec(const std::string_view instance, const std::vector<std::string> &command, const exec_options &options) const {
        if (const auto *other = route(instance, options.project)) return other->exec(instance, command, options);
        if (command.empty()) throw std::invalid_argument("sc::lxd::client::exec: no command");
        nlohmann::json order{{"command", command}, {"wait-for-websocket", false}, {"interactive", false}, {"record-output", true}};
        order["environment"] = nlohmann::json::object();
        for (const auto &[name, value]: options.environment) order["environment"][name] = value;
        if (options.user) order["user"] = *options.user;
        if (options.group) order["group"] = *options.group;
        if (!options.folder.empty()) order["cwd"] = options.folder;
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
        // Each recorded stream read and removed, stderr's beside stdout's: a busy cluster takes up to a
        // second or two for each of these requests, so the exec waits for two of them, not four.
        const auto collect = [this, &output](const char *stream) {
            const auto log = text_at(output, stream);
            if (log.empty()) return std::string{};
            auto body = request("GET", log, nullptr, true).body;
            try {
                (void) metadata_of(request("DELETE", log));
            } catch (const std::exception &) {
                // A log left behind is LXD's to clear (at the instance's next start); the result stands.
            }
            return body;
        };
        auto err = std::async(std::launch::async, collect, "2");
        out.out = collect("1");
        out.err = err.get();
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
        const auto name = body.value("name", std::string{});
        if (const auto *other = standalone_named(target)) {
            other->create(body, project, {}, limit);
            place(project, name, target);
            return;
        }
        const auto text = body.dump();
        const auto started = metadata_of(request("POST", "/1.0/instances?project=" + encoded(project) +
                                                             (target.empty() ? std::string{} : "&target=" + encoded(target)), &text));
        wait_for(started, project, limit, "making " + name);
    }

    void client::change_state(const std::string_view instance, const std::string_view action, const std::string_view project, const bool force,
                              const std::chrono::seconds limit) const {
        if (const auto *other = route(instance, project)) return other->change_state(instance, action, project, force, limit);
        const auto text = nlohmann::json{{"action", action}, {"timeout", static_cast<int>(limit.count())}, {"force", force}}.dump();
        const auto started = metadata_of(request("PUT", "/1.0/instances/" + encoded(instance) + "/state?project=" + encoded(project), &text));
        wait_for(started, project, limit, std::string{action} + " " + std::string{instance});
    }

    bool client::update(const std::string_view instance, const std::string_view project, const std::chrono::seconds limit, const std::string &what,
                        const std::function<bool(nlohmann::json &)> &change) const {
        if (const auto *other = route(instance, project)) return other->update(instance, project, limit, what, change);
        const auto path = "/1.0/instances/" + encoded(instance) + "?project=" + encoded(project);
        for (int attempt = 1;; ++attempt) {
            const auto read = request("GET", path);
            const auto current = metadata_of(read);
            // What PUT takes (InstancePut), as it is now.
            nlohmann::json body = nlohmann::json::object();
            for (const char *key: {"architecture", "config", "devices", "ephemeral", "profiles", "stateful", "description"})
                if (current.contains(key)) body[key] = current[key];
            for (const char *key: {"config", "devices"})
                if (!body.contains(key) || !body[key].is_object()) body[key] = nlohmann::json::object();
            if (!change(body)) return false;
            std::map<std::string, std::string> headers;
            if (const auto tag = read.headers.find("etag"); tag != read.headers.end()) headers["If-Match"] = tag->second;
            const auto text = body.dump();
            try {
                const auto answer = request("PUT", path, &text, false, std::chrono::seconds{0}, &headers);
                const auto started = metadata_of(answer);
                if (nlohmann::json::parse(answer.body, nullptr, false).value("type", "") == "async") wait_for(started, project, limit, what);
                return true;
            } catch (const lxd::error &refused) {
                if (refused.status() != 412 || attempt == 3) throw; // 412: changed since it was read
            }
        }
    }

    void client::set_config(const std::string_view instance, const std::map<std::string, std::string> &values, const std::string_view project,
                            const std::chrono::seconds limit) const {
        (void) update(instance, project, limit, "changing " + std::string{instance} + "'s config", [&](nlohmann::json &body) {
            for (const auto &[key, value]: values) body["config"][key] = value;
            return true;
        });
    }

    std::optional<nlohmann::json> client::remove_device(const std::string_view instance, const std::string_view device, const std::string_view project,
                                                        const std::chrono::seconds limit) const {
        std::optional<nlohmann::json> removed;
        (void) update(instance, project, limit, "removing " + std::string{device} + " from " + std::string{instance}, [&](nlohmann::json &body) {
            removed.reset();
            const auto found = body["devices"].find(std::string{device});
            if (found == body["devices"].end()) return false;
            removed = *found;
            body["devices"].erase(found);
            return true;
        });
        return removed;
    }

    client::device_change client::set_device(const std::string_view instance, const std::string_view device, const nlohmann::json &properties,
                                             const std::string_view project, const std::chrono::seconds limit) const {
        device_change out;
        out.changed = update(instance, project, limit, "setting " + std::string{device} + " of " + std::string{instance}, [&](nlohmann::json &body) {
            out.before.reset();
            const auto found = body["devices"].find(std::string{device});
            if (found != body["devices"].end()) {
                out.before = *found;
                if (*found == properties) return false;
            }
            body["devices"][std::string{device}] = properties;
            return true;
        });
        return out;
    }

    bool client::remove(const std::string_view instance, const std::string_view project, const std::chrono::seconds limit) const {
        if (const auto *other = route(instance, project)) {
            const bool removed = other->remove(instance, project, limit);
            const std::lock_guard lock{route_mutex_};
            for (auto &each: standalone_)
                if (each.lxd.get() == other) each.seen.erase(key_of(project, instance));
            return removed;
        }
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
