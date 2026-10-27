// sc::lxd::instance from LXD's JSON: state, addresses, usage; what's missing stays empty.
#include <sc_lxd.h>

#include "sc_test.h"

#include <fstream>

int main() {
    std::ifstream file{SC_LXD_TEST_RESOURCE "/instances.json"};
    const auto listed = nlohmann::json::parse(file);

    SECTION("A running container: its state, global IPv4 addresses on every interface but lo, usage");
    {
        const auto web = sc::lxd::instance::from_json(listed[0]);
        CHECK_EQ(web.name, std::string{"web1"});
        CHECK_EQ(web.project, std::string{"default"});
        CHECK_EQ(web.location, std::string{"hv2"});
        CHECK_EQ(web.type, std::string{"container"});
        CHECK_EQ(web.description, std::string{"Apache"});
        CHECK_EQ(web.status, std::string{"Running"});
        CHECK_EQ(web.status_code, 103);
        CHECK_EQ(web.ipv4.size(), std::size_t{2});
        CHECK(web.ipv4 == std::vector<std::string>({"10.0.100.21", "10.0.105.21"}));
        CHECK(web.memory_usage && *web.memory_usage == 734003200);
        CHECK(web.disk_usage && *web.disk_usage == 5368709120);
        CHECK(web.swap_usage && *web.swap_usage == 1048576);
        CHECK(web.cpu_usage && *web.cpu_usage == 23248148000);
        CHECK(web.processes && *web.processes == 39);
        CHECK_EQ(web.created_at, std::string{"2024-03-01T10:00:00Z"});
        CHECK_EQ(web.last_used_at, std::string{"2026-10-20T06:00:00Z"});
    }

    SECTION("A stopped one: no memory in use, no disk figure, no addresses");
    {
        const auto old = sc::lxd::instance::from_json(listed[1]);
        CHECK_EQ(old.status_code, 102);
        CHECK(!old.memory_usage);
        CHECK(!old.disk_usage);
        CHECK(!old.swap_usage && !old.cpu_usage && !old.processes);
        CHECK(old.ipv4.empty());
    }

    SECTION("Without state: what the instance itself says");
    {
        auto bare = listed[0];
        bare.erase("state");
        bare["status"] = "Frozen";
        bare["status_code"] = 110;
        const auto frozen = sc::lxd::instance::from_json(bare);
        CHECK_EQ(frozen.status, std::string{"Frozen"});
        CHECK_EQ(frozen.status_code, 110);
        CHECK(frozen.ipv4.empty());
        CHECK(!frozen.memory_usage);
    }

    TEST_SUMMARY();
}
