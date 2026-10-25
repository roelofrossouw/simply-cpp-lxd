// sc::lxd::client against a stand-in LXD member (resource/lxd_server.py, HTTPS wanting a client
// certificate): members tried in turn, LXD's errors, an unknown instance, nobody answering.
#include <sc_lxd.h>

#include "sc_test.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;

int main() {
    if (std::system("openssl version > /dev/null 2>&1") != 0 || std::system("python3 --version > /dev/null 2>&1") != 0) {
        std::cout << "   (no openssl or python3: not tested)" << std::endl;
        TEST_SUMMARY();
    }
    const auto dir = fs::temp_directory_path() / ("sc-lxd-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const auto at = [&](const std::string &name) { return (dir / name).string(); };
    for (const std::string name: {"server", "client", "stranger"})
        CHECK_EQ(std::system(("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 -subj /CN=sc-lxd-" + name +
                              " -keyout " + at(name + ".key") + " -out " + at(name + ".crt") + " > /dev/null 2>&1").c_str()), 0);
    const int port = 19443 + static_cast<int>(std::random_device{}() % 1000);
    CHECK_EQ(std::system(("python3 " SC_LXD_TEST_RESOURCE "/lxd_server.py " + std::to_string(port) + " " + at("server.crt") + " " +
                          at("server.key") + " " + at("client.crt") + " " SC_LXD_TEST_RESOURCE "/instances.json > " + at("server.log") +
                          " 2>&1 & echo $! > " + at("server.pid")).c_str()), 0);
    const std::string live = "https://127.0.0.1:" + std::to_string(port);
    const std::string dead = "https://127.0.0.1:9"; // nothing listens on the discard port
    const sc::lxd::credentials known{.certificate = at("client.crt"), .key = at("client.key"), .server_certificate = at("server.crt")};
    for (int tries = 0; tries < 100; ++tries) { // until it listens
        try {
            (void) sc::lxd::client{{live}, known, std::chrono::seconds{2}}.instances();
            break;
        } catch (const std::exception &) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    SECTION("Members tried in turn: a dead one skipped, the one that answered asked first after");
    {
        const sc::lxd::client lxd{{dead, live + "/"}, known, std::chrono::seconds{2}};
        CHECK_EQ(lxd.member(), dead);
        const auto all = lxd.instances();
        CHECK_EQ(all.size(), std::size_t{2});
        CHECK_EQ(all[0].name, std::string{"web1"});
        CHECK_EQ(all[0].ipv4.size(), std::size_t{2});
        CHECK_EQ(lxd.member(), live); // its trailing / dropped
        CHECK_EQ(lxd.instances().size(), std::size_t{2});
    }

    SECTION("One instance by name; nothing for an unknown one; LXD's error otherwise");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        const auto web = lxd.find("web1");
        CHECK(web && web->location == "hv2" && web->status_code == 103);
        CHECK(!lxd.find("web1", "other-project"));
        CHECK(!lxd.find("no such/name"));
        CHECK_THROWS_AS((void) lxd.get("/1.0/networks"), sc::lxd::error);
        try {
            (void) lxd.get("/1.0/networks");
        } catch (const sc::lxd::error &refused) {
            CHECK_EQ(refused.status(), 404);
        }
    }

    SECTION("Files: a file with its owner and mode, a symlink, a directory, nothing for a missing path");
    {
        const sc::lxd::client lxd{{dead, live}, known, std::chrono::seconds{2}};
        const auto hostname = lxd.read_file("web1", "/etc/hostname");
        CHECK(hostname && hostname->type == "file");
        CHECK_EQ(hostname->content, std::string{"web1\n"});
        CHECK_EQ(hostname->mode, 0644u);
        CHECK_EQ(hostname->uid, 0);
        const auto sudoers = lxd.read_file("web1", "/etc/sudoers");
        CHECK(sudoers && sudoers->mode == 0440u);
        CHECK_EQ(sudoers->content, std::string("root ALL=(ALL) ALL\n\0", 20)); // bytes, a zero among them
        const auto localtime = lxd.read_file("web1", "/etc/localtime");
        CHECK(localtime && localtime->type == "symlink");
        CHECK_EQ(localtime->content, std::string{"/usr/share/zoneinfo/Africa/Johannesburg"});
        const auto apt = lxd.read_file("web1", "/etc/apt");
        CHECK(apt && apt->type == "directory" && apt->content.empty());
        CHECK_EQ(apt->entries.size(), std::size_t{2});
        CHECK(!lxd.read_file("web1", "/etc/no such file"));
        CHECK_EQ(lxd.member(), live);
    }

    SECTION("exec: exit status and output, LXD's recorded output removed; refused; too slow");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        const auto ran = lxd.exec("web1", {"sh", "-c", "echo hi"}, {.environment = {{"LANG", "C.UTF-8"}}});
        CHECK_EQ(ran.status, 0);
        CHECK_EQ(ran.out, std::string{"sh -c echo hi LANG=C.UTF-8\n"});
        CHECK_EQ(ran.err, std::string{"warned\n"});
        CHECK_EQ(lxd.exec("web1", {"fail"}).status, 3);
        // The stand-in forgets a log once it's deleted: none is left.
        CHECK_THROWS_AS((void) lxd.get("/1.0/instances/web1/logs/exec-output/none"), sc::lxd::error);
        try {
            (void) lxd.exec("web2", {"true"});
            CHECK_MSG(false, "exec in an instance that isn't running didn't throw");
        } catch (const sc::lxd::error &refused) {
            CHECK_EQ(refused.status(), 400);
        }
        try {
            (void) lxd.exec("web1", {"sleep", "100"}, {.limit = std::chrono::seconds{1}});
            CHECK_MSG(false, "a command past its limit didn't throw");
        } catch (const sc::lxd::error &slow) {
            CHECK_EQ(slow.status(), 408);
        }
        CHECK_THROWS_AS((void) lxd.exec("web1", {}), std::invalid_argument);
    }

    SECTION("Nobody answering, a certificate the server doesn't know, a server not trusted");
    {
        CHECK_THROWS_AS((void) sc::lxd::client({dead}, known, std::chrono::seconds{2}).instances(), sc::lxd::unreachable);
        const sc::lxd::credentials stranger{.certificate = at("stranger.crt"), .key = at("stranger.key"), .server_certificate = at("server.crt")};
        CHECK_THROWS_AS((void) sc::lxd::client({live}, stranger, std::chrono::seconds{2}).instances(), sc::lxd::unreachable);
        const sc::lxd::credentials untrusting{.certificate = at("client.crt"), .key = at("client.key"), .server_certificate = at("stranger.crt")};
        CHECK_THROWS_AS((void) sc::lxd::client({live}, untrusting, std::chrono::seconds{2}).instances(), sc::lxd::unreachable);
        try {
            (void) sc::lxd::client({dead, live}, untrusting, std::chrono::seconds{2}).instances();
        } catch (const sc::lxd::unreachable &none) {
            const std::string why = none.what();
            CHECK_MSG(why.find(dead) != std::string::npos && why.find(live) != std::string::npos, why); // each member's reason
        }
    }

    SECTION("Mistakes");
    {
        CHECK_THROWS_AS(sc::lxd::client({}, known), std::invalid_argument);
        CHECK_THROWS_AS(sc::lxd::client({live}, sc::lxd::credentials{}), std::invalid_argument);
    }

    (void) std::system(("kill $(cat " + at("server.pid") + ") > /dev/null 2>&1").c_str());
    fs::remove_all(dir);
    TEST_SUMMARY();
}
