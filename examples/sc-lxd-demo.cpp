// Lists a cluster's instances, shows a file in one, or runs a command in one:
//     sc-lxd-demo CERT KEY SERVER_CERT MEMBER_URL...
//     sc-lxd-demo --file web1:/etc/hostname CERT KEY SERVER_CERT MEMBER_URL...
//     sc-lxd-demo --exec web1 --command "uptime" CERT KEY SERVER_CERT MEMBER_URL...
#include <sc_lxd.h>

#include <params.h>

#include <iomanip>
#include <iostream>

namespace {
    void list(const sc::lxd::client &lxd) {
        for (const auto &each: lxd.instances()) {
            std::cout << each.project << '/' << each.name << " on " << each.location << ": " << each.status;
            for (const auto &address: each.ipv4) std::cout << ' ' << address;
            if (each.memory_usage) std::cout << ", " << *each.memory_usage / (1024 * 1024) << " MiB";
            std::cout << '\n';
        }
    }

    int show(const sc::lxd::client &lxd, const std::string &where) {
        const auto colon = where.find(':');
        if (colon == std::string::npos) throw std::invalid_argument("--file wants INSTANCE:PATH");
        const auto found = lxd.read_file(where.substr(0, colon), where.substr(colon + 1));
        if (!found) {
            std::cerr << where << ": not there" << std::endl;
            return 1;
        }
        std::cout << found->type << ", mode " << std::oct << std::setfill('0') << std::setw(4) << found->mode << std::dec << ", owner "
                  << found->uid << ':' << found->gid << '\n';
        if (found->type == "directory") {
            for (const auto &name: found->entries) std::cout << name << '\n';
        } else if (found->type == "symlink") {
            std::cout << "-> " << found->content << '\n';
        } else {
            std::cout << found->content;
        }
        return 0;
    }

    int run(const sc::lxd::client &lxd, const std::string &instance, const std::string &command) {
        const auto ended = lxd.exec(instance, {"sh", "-c", command}, {.environment = {{"LANG", "C.UTF-8"}}});
        std::cout << ended.out;
        std::cerr << ended.err;
        return ended.status;
    }
}

int main(int argc, char *argv[]) {
    const sc::params args{argc, argv, nlohmann::ordered_json{
        {"name", "sc-lxd-demo"},
        {"description", "Lists an LXD cluster's instances with their state, as the client certificate sees them; or shows a file in "
                        "an instance, or runs a command in one."},
        {"options", {
            {"file", {{"value", "INSTANCE:PATH"}, {"help", "show this file (or symlink, or directory) with its mode and owner"}}},
            {"exec", {{"value", "INSTANCE"}, {"help", "run --command in this instance"}}},
            {"command", {{"value", "TEXT"}, {"default", "uname -a"}, {"help", "the command for --exec, run by sh -c"}}}}},
        {"positional", {
            {"certificate", {{"help", "the client's certificate (PEM), one the cluster trusts"}}},
            {"key", {{"help", "its key (PEM)"}}},
            {"server", {{"help", "the cluster's certificate (PEM), the server known by it"}}}}},
        {"extra", {{"members", {{"required", true}, {"help", "the members' URLs (https://10.0.100.3:8443)"}}}}}}};
    try {
        const sc::lxd::client lxd{args["members"].get<std::vector<std::string>>(),
                                  {.certificate = args["certificate"].get<std::string>(), .key = args["key"].get<std::string>(),
                                   .server_certificate = args["server"].get<std::string>()}};
        int status = 0;
        if (const auto *file = args.lookup("file")) {
            status = show(lxd, file->get<std::string>());
        } else if (const auto *instance = args.lookup("exec")) {
            status = run(lxd, instance->get<std::string>(), args.as<std::string>("command"));
        } else {
            list(lxd);
        }
        std::cerr << "(answered by " << lxd.member() << ")\n";
        return status;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
