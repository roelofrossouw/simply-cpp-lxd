// sc::lxd::client with a standalone server beside the cluster (4.0.0's standalone hosts): two stand-in
// LXD servers (resource/lxd_server.py), one the cluster, one a standalone host "hv3" with its own
// certificate. Calls go where the instance is, listings merge, a pinned instance goes to its host,
// create() with the host as target makes it there, a host that doesn't answer is named, not fatal.
#include <sc/lxd/lxd.h>

#include "sc_test.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {
    const sc::lxd::instance *named(const std::vector<sc::lxd::instance> &all, const std::string &name) {
        const auto found = std::ranges::find_if(all, [&](const sc::lxd::instance &each) { return each.name == name; });
        return found == all.end() ? nullptr : &*found;
    }
}

int main() {
    if (std::system("openssl version > /dev/null 2>&1") != 0 || std::system("python3 --version > /dev/null 2>&1") != 0) {
        std::cout << "   (no openssl or python3: not tested)" << std::endl;
        TEST_SUMMARY();
    }
    const auto dir = fs::temp_directory_path() / ("sc-lxd-standalone-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const auto at = [&](const std::string &name) { return (dir / name).string(); };
    for (const std::string name: {"cluster", "hv3", "client"})
        CHECK_EQ(std::system(("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -pkeyopt ec_param_enc:named_curve -nodes -days 1 -subj /CN=sc-lxd-" + name +
                              " -addext basicConstraints=critical,CA:TRUE -addext subjectKeyIdentifier=hash -addext authorityKeyIdentifier=keyid:always"
                              " -addext keyUsage=critical,digitalSignature,keyCertSign -addext subjectAltName=IP:127.0.0.1"
                              " -keyout " + at(name + ".key") + " -out " + at(name + ".crt") + " > /dev/null 2>&1").c_str()), 0);
    // Each server gets its own copy of its instances (the stand-in changes them).
    fs::copy_file(SC_LXD_TEST_RESOURCE "/instances.json", at("cluster.json"));
    fs::copy_file(SC_LXD_TEST_RESOURCE "/standalone.json", at("hv3.json"));
    const auto start = [&](const std::string &name) {
        CHECK_EQ(std::system(("python3 " SC_LXD_TEST_RESOURCE "/lxd_server.py 0 " + at(name + ".crt") + " " + at(name + ".key") + " " +
                              at("client.crt") + " " + at(name + ".json") + " > " + at(name + ".log") + " 2>&1 & echo $! > " + at(name + ".pid"))
                                 .c_str()),
                 0);
        int port = 0;
        for (int tries = 0; tries < 100 && port == 0; ++tries) {
            std::ifstream log{at(name + ".log")};
            std::string word;
            if (log >> word && word == "listening") log >> port;
            if (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        CHECK(port != 0);
        return "https://127.0.0.1:" + std::to_string(port);
    };
    const auto cluster_url = start("cluster");
    const auto hv3_url = start("hv3");
    const sc::lxd::credentials cluster_trust{.certificate = at("client.crt"), .key = at("client.key"), .server_certificate = at("cluster.crt")};
    const sc::lxd::credentials hv3_trust{.certificate = at("client.crt"), .key = at("client.key"), .server_certificate = at("hv3.crt")};
    for (int tries = 0; tries < 100; ++tries) { // until both listen
        try {
            (void) sc::lxd::client{{cluster_url}, cluster_trust, std::chrono::seconds{2}}.instances();
            (void) sc::lxd::client{{hv3_url}, hv3_trust, std::chrono::seconds{2}}.instances();
            break;
        } catch (const std::exception &) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
    const auto with_hv3 = [&](const std::string &url) {
        return sc::lxd::client{{cluster_url}, cluster_trust, {{.name = "hv3", .urls = {url}, .how = hv3_trust}}, std::chrono::seconds{2}};
    };

    SECTION("Without standalone hosts: the cluster's alone, as before");
    {
        const sc::lxd::client lxd{{cluster_url}, cluster_trust, std::chrono::seconds{2}};
        CHECK(lxd.hosts().empty());
        CHECK(named(lxd.instances(), "web1") != nullptr);
        CHECK(named(lxd.instances(), "mail3") == nullptr);
        CHECK_EQ(lxd.host_of("mail3"), std::string{});
        CHECK(!lxd.find("mail3").has_value());
        lxd.place("default", "mail3", "hv3"); // no such host: nothing pinned
        CHECK(!lxd.find("mail3").has_value());
    }

    SECTION("Listings merged: the standalone host's instances with its name as their location");
    {
        const auto lxd = with_hv3(hv3_url);
        CHECK_EQ(lxd.hosts().size(), 1u);
        const auto all = lxd.list();
        CHECK(all.unanswered.empty());
        CHECK(named(all.instances, "web1") != nullptr);
        CHECK(named(all.instances, "mail3") != nullptr);
        if (const auto *mail3 = named(all.instances, "mail3")) {
            CHECK_EQ(mail3->location, std::string{"hv3"});
            CHECK_EQ(mail3->status_code, 103);
            CHECK_EQ(mail3->ipv4.size(), 1u);
        }
        CHECK_EQ(lxd.instances().size(), all.instances.size());
        // The raw list too (what a job reads with get()).
        const auto raw = lxd.get("/1.0/instances?recursion=2&all-projects=true");
        CHECK(raw.is_array());
        bool found = false;
        for (const auto &each: raw)
            if (each.value("name", "") == "mail3") found = each.value("location", "") == "hv3";
        CHECK(found);
        CHECK(lxd.on_host("hv3").contains("default/mail3"));
    }

    SECTION("Calls go where the instance is: by the host's listing, or pinned");
    {
        const auto lxd = with_hv3(hv3_url);
        CHECK_EQ(lxd.host_of("mail3"), std::string{"hv3"});
        CHECK_EQ(lxd.host_of("web1"), std::string{});
        const auto mail3 = lxd.find("mail3");
        CHECK(mail3.has_value());
        if (mail3) CHECK_EQ(mail3->location, std::string{"hv3"});
        CHECK(lxd.find("web1").has_value());
        const auto one = lxd.get("/1.0/instances/mail3?project=default");
        CHECK_EQ(one.value("location", ""), std::string{"hv3"});
        // Pinned: web1's calls go to hv3, which doesn't have it.
        lxd.place("default", "web1", "hv3");
        CHECK_EQ(lxd.host_of("web1"), std::string{"hv3"});
        CHECK(!lxd.find("web1").has_value());
        CHECK(lxd.on_host("hv3").contains("default/web1"));
        lxd.place("default", "web1", "");
        CHECK(lxd.find("web1").has_value());
    }

    SECTION("create() with the standalone host as target: made there, pinned; removed there");
    {
        const auto lxd = with_hv3(hv3_url);
        lxd.create({{"name", "smtp3"}, {"type", "container"}, {"source", {{"type", "image"}, {"alias", "ubuntu"}}}}, "default", "hv3");
        CHECK_EQ(lxd.host_of("smtp3"), std::string{"hv3"});
        CHECK(sc::lxd::client({hv3_url}, hv3_trust, std::chrono::seconds{2}).find("smtp3").has_value());
        CHECK(!sc::lxd::client({cluster_url}, cluster_trust, std::chrono::seconds{2}).find("smtp3").has_value());
        lxd.change_state("smtp3", "start");
        CHECK(lxd.remove("smtp3"));
        CHECK(!sc::lxd::client({hv3_url}, hv3_trust, std::chrono::seconds{2}).find("smtp3").has_value());
        // A cluster member as target: the cluster's, as before.
        lxd.create({{"name", "web9"}, {"type", "container"}, {"source", {{"type", "image"}, {"alias", "ubuntu"}}}}, "default", "hv2");
        CHECK_EQ(lxd.host_of("web9"), std::string{});
        CHECK(sc::lxd::client({cluster_url}, cluster_trust, std::chrono::seconds{2}).find("web9").has_value());
        CHECK(lxd.remove("web9"));
    }

    SECTION("A standalone host that doesn't answer: named, the cluster's listing stands; its pinned instances fail, not fall back");
    {
        const auto lxd = with_hv3("https://127.0.0.1:9"); // nothing listens on the discard port
        const auto all = lxd.list();
        CHECK_EQ(all.unanswered.size(), 1u);
        if (!all.unanswered.empty()) CHECK_EQ(all.unanswered.front(), std::string{"hv3"});
        CHECK(named(all.instances, "web1") != nullptr);
        CHECK(named(all.instances, "mail3") == nullptr);
        lxd.place("default", "mail3", "hv3");
        CHECK_THROWS_AS(lxd.find("mail3"), std::exception);
        // An instance it doesn't know of: the cluster's.
        CHECK(lxd.find("web1").has_value());
    }

    SECTION("Mistakes");
    {
        CHECK_THROWS_AS(sc::lxd::client({cluster_url}, cluster_trust, {{.name = "", .urls = {hv3_url}, .how = hv3_trust}}), std::exception);
        CHECK_THROWS_AS(sc::lxd::client({cluster_url}, cluster_trust, {{.name = "hv3", .urls = {}, .how = hv3_trust}}), std::exception);
        CHECK_THROWS_AS(sc::lxd::client({cluster_url}, cluster_trust,
                                        {{.name = "hv3", .urls = {hv3_url}, .how = hv3_trust}, {.name = "hv3", .urls = {hv3_url}, .how = hv3_trust}}),
                        std::exception);
    }

    for (const std::string name: {"cluster", "hv3"}) (void) std::system(("kill $(cat " + at(name + ".pid") + ") > /dev/null 2>&1").c_str());
    fs::remove_all(dir);
    TEST_SUMMARY();
}
