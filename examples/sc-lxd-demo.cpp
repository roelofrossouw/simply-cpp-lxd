// Lists a cluster's instances: sc-lxd-demo CERT KEY SERVER_CERT MEMBER_URL...
#include <sc_lxd.h>

#include <params.h>

#include <iostream>

int main(int argc, char *argv[]) {
    const sc::params args{argc, argv, nlohmann::ordered_json{
        {"name", "sc-lxd-demo"},
        {"description", "Lists an LXD cluster's instances with their state, as the client certificate sees them."},
        {"positional", {
            {"certificate", {{"help", "the client's certificate (PEM), one the cluster trusts"}}},
            {"key", {{"help", "its key (PEM)"}}},
            {"server", {{"help", "the cluster's certificate (PEM), the server known by it"}}}}},
        {"extra", {{"members", {{"required", true}, {"help", "the members' URLs (https://10.0.100.3:8443)"}}}}}}};
    try {
        const sc::lxd::client lxd{args["members"].get<std::vector<std::string>>(),
                                  {.certificate = args["certificate"].get<std::string>(), .key = args["key"].get<std::string>(),
                                   .server_certificate = args["server"].get<std::string>()}};
        for (const auto &each: lxd.instances()) {
            std::cout << each.project << '/' << each.name << " on " << each.location << ": " << each.status;
            for (const auto &address: each.ipv4) std::cout << ' ' << address;
            if (each.memory_usage) std::cout << ", " << *each.memory_usage / (1024 * 1024) << " MiB";
            std::cout << '\n';
        }
        std::cout << "(answered by " << lxd.member() << ")\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
