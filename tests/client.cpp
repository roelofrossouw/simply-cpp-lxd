// sc::lxd::client against a stand-in LXD member (resource/lxd_server.py, HTTPS wanting a client
// certificate): members tried in turn, LXD's errors, an unknown instance, nobody answering.
#include <sc_lxd.h>

#include "sc_test.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    // The curve named and the extensions spelt out: macOS's own openssl (LibreSSL) otherwise writes
    // the key's curve out in full and adds no extensions, and TLS with OpenSSL 3 refuses both.
    for (const std::string name: {"server", "client", "stranger"})
        CHECK_EQ(std::system(("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -pkeyopt ec_param_enc:named_curve -nodes -days 1 -subj /CN=sc-lxd-" + name +
                              " -addext basicConstraints=critical,CA:TRUE -addext subjectKeyIdentifier=hash -addext authorityKeyIdentifier=keyid:always"
                              " -addext keyUsage=critical,digitalSignature,keyCertSign -addext subjectAltName=IP:127.0.0.1"
                              " -keyout " + at(name + ".key") + " -out " + at(name + ".crt") + " > /dev/null 2>&1").c_str()), 0);
    // A free port of the system's choice, which the server says ("listening PORT"): a port picked
    // here could be one something else listens on.
    CHECK_EQ(std::system(("python3 " SC_LXD_TEST_RESOURCE "/lxd_server.py 0 " + at("server.crt") + " " +
                          at("server.key") + " " + at("client.crt") + " " SC_LXD_TEST_RESOURCE "/instances.json > " + at("server.log") +
                          " 2>&1 & echo $! > " + at("server.pid")).c_str()), 0);
    int port = 0;
    for (int tries = 0; tries < 100 && port == 0; ++tries) {
        std::ifstream log{at("server.log")};
        std::string word;
        if (log >> word && word == "listening") log >> port;
        if (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    CHECK(port != 0);
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

    SECTION("Files written: a file with owner and mode, a symlink, replaced, removed; a missing folder refused");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        lxd.write_file("web1", "/etc/69-oneweb.conf", {.type = "file", .content = std::string("a = 1\n\0", 7), .uid = 33, .gid = 4, .mode = 0640});
        const auto written = lxd.read_file("web1", "/etc/69-oneweb.conf");
        CHECK(written && written->type == "file");
        if (written) {
            CHECK_EQ(written->content, std::string("a = 1\n\0", 7));
            CHECK_EQ(written->mode, 0640u);
            CHECK_EQ(written->uid, 33);
            CHECK_EQ(written->gid, 4);
        }
        lxd.write_file("web1", "/etc/69-oneweb.conf", {.type = "file", .content = "a = 2\n", .mode = 0644}); // replaced
        const auto replaced = lxd.read_file("web1", "/etc/69-oneweb.conf");
        CHECK(replaced && replaced->content == "a = 2\n" && replaced->mode == 0644u && replaced->uid == 0);
        lxd.write_file("web1", "/etc/current", {.type = "symlink", .content = "/etc/69-oneweb.conf"});
        const auto link = lxd.read_file("web1", "/etc/current");
        CHECK(link && link->type == "symlink" && link->content == "/etc/69-oneweb.conf");
        CHECK(lxd.delete_file("web1", "/etc/69-oneweb.conf"));
        CHECK(!lxd.read_file("web1", "/etc/69-oneweb.conf"));
        CHECK(!lxd.delete_file("web1", "/etc/69-oneweb.conf")); // nothing there any more
        try {
            lxd.write_file("web1", "/etc/nowhere.d/x.conf", {.type = "file", .content = "x"});
            CHECK_MSG(false, "a write into a missing folder didn't throw");
        } catch (const sc::lxd::error &refused) {
            CHECK_EQ(refused.status(), 404);
        }
        CHECK_THROWS_AS(lxd.write_file("web1", "/etc/x", {.type = "pipe"}), std::invalid_argument);
    }

    SECTION("exec: exit status and output, LXD's recorded output removed; refused; too slow");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        const auto ran = lxd.exec("web1", {"sh", "-c", "echo hi"}, {.environment = {{"LANG", "C.UTF-8"}}});
        CHECK_EQ(ran.status, 0);
        CHECK_EQ(ran.out, std::string{"sh -c echo hi LANG=C.UTF-8\n"});
        CHECK_EQ(ran.err, std::string{"warned\n"});
        CHECK_EQ(lxd.exec("web1", {"fail"}).status, 3);
        // As another user, in a folder: asked of LXD as user, group and cwd.
        CHECK_EQ(lxd.exec("web1", {"id"}, {.user = 48, .group = 48, .folder = "/home/test"}).out, std::string{"id user:48 group:48 cwd:/home/test\n"});
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

    SECTION("Instances made, started, stopped and removed; a name taken; a failed image; removing what isn't there");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        lxd.create({{"name", "new1"}, {"type", "container"}, {"profiles", {"default"}}, {"config", {{"limits.cpu", "2"}}},
                    {"source", {{"type", "image"}, {"protocol", "simplestreams"}, {"server", "https://cloud-images.ubuntu.com/releases"},
                                {"alias", "resolute"}}}},
                   "default", "hv2");
        auto made = lxd.find("new1");
        CHECK(made.has_value());
        if (made) {
            CHECK_EQ(made->location, std::string{"hv2"});
            CHECK_EQ(made->status_code, 102);
        }
        lxd.change_state("new1", "start");
        made = lxd.find("new1");
        CHECK(made && made->status_code == 103);
        CHECK(made && made->ipv4 == std::vector<std::string>{"10.0.100.250"});
        try {
            lxd.create({{"name", "new1"}, {"source", {{"type", "image"}, {"alias", "resolute"}}}});
            CHECK_MSG(false, "making an instance of a name there is didn't throw");
        } catch (const sc::lxd::error &taken) {
            CHECK_EQ(taken.status(), 409);
        }
        try {
            lxd.create({{"name", "new2"}, {"source", {{"type", "image"}, {"alias", "broken"}}}});
            CHECK_MSG(false, "a failed image didn't throw");
        } catch (const sc::lxd::error &failed) {
            CHECK_EQ(failed.status(), 500);
            CHECK(std::string{failed.what()}.find("image not found") != std::string::npos);
        }
        CHECK(!lxd.find("new2").has_value());
        CHECK(lxd.remove("new1")); // running: stopped first
        CHECK(!lxd.find("new1").has_value());
        CHECK(!lxd.remove("new1"));
    }

    SECTION("Config keys set, the others kept; a value LXD refuses; no such instance");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        lxd.set_config("web1", {{"limits.cpu", "4"}, {"raw.idmap", "both 33-2000 33-2000"}});
        lxd.set_config("web1", {{"limits.memory", "4GiB"}});
        const auto config = lxd.get("/1.0/instances/web1")["config"];
        CHECK_EQ(config.value("limits.cpu", ""), std::string{"4"});
        CHECK_EQ(config.value("raw.idmap", ""), std::string{"both 33-2000 33-2000"});
        CHECK_EQ(config.value("limits.memory", ""), std::string{"4GiB"});
        try {
            lxd.set_config("web1", {{"limits.cpu", "many"}});
            CHECK_MSG(false, "a value LXD refuses didn't throw");
        } catch (const sc::lxd::error &refused) {
            CHECK_EQ(refused.status(), 400);
        }
        CHECK_EQ(lxd.get("/1.0/instances/web1")["config"].value("limits.cpu", ""), std::string{"4"});
        try {
            lxd.set_config("none", {{"limits.cpu", "1"}});
            CHECK_MSG(false, "no such instance didn't throw");
        } catch (const sc::lxd::error &missing) {
            CHECK_EQ(missing.status(), 404);
        }
    }

    SECTION("A device taken out, the others and the config kept; one that isn't there");
    {
        const sc::lxd::client lxd{{live}, known, std::chrono::seconds{2}};
        const auto removed = lxd.remove_device("web1", "data");
        CHECK(removed && removed->value("source", "") == "/srv/data");
        const auto after = lxd.get("/1.0/instances/web1");
        CHECK(!after["devices"].contains("data"));
        CHECK(after["devices"].contains("eth0"));
        CHECK_EQ(after["config"].value("limits.cpu", ""), std::string{"4"}); // set_config's, above
        CHECK(!lxd.remove_device("web1", "data"));
        const nlohmann::json logs{{"type", "disk"}, {"source", "/srv/logs"}, {"path", "/logs"}};
        auto set = lxd.set_device("web1", "logs", logs);
        CHECK(set.changed && !set.before);
        set = lxd.set_device("web1", "logs", logs); // as it is already
        CHECK(!set.changed && set.before && *set.before == logs);
        set = lxd.set_device("web1", "logs", {{"type", "disk"}, {"source", "/srv/logs"}, {"path", "/var/logs"}});
        CHECK(set.changed && set.before && set.before->value("path", "") == "/logs");
        CHECK_EQ(lxd.get("/1.0/instances/web1")["devices"]["logs"].value("path", ""), std::string{"/var/logs"});
        CHECK(lxd.get("/1.0/instances/web1")["devices"].contains("eth0"));
        CHECK_THROWS_AS((void) lxd.remove_device("none", "data"), sc::lxd::error);
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
