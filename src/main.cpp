#include "app.hpp"
#include "launcher/launcher.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

static void usage(const char* argv0) {
    fprintf(stderr,
            "fastrdp — low-latency RDP client\n\n"
            "Usage: %s                      open the connection manager\n"
            "       %s [options] /v:host[:port] /u:user [/d:domain] [FreeRDP options]\n\n"
            "fastrdp options:\n"
            "  --vsync           wait for vblank on present (default: present immediately)\n"
            "  --no-h264         don't offer H.264/AVC444 to the server\n"
            "  --sw-decode       decode H.264 on the CPU instead of VAAPI\n"
            "  --scale=MODE      fit (default), stretch, or native while sizes differ\n"
            "  --stats           print per-second performance stats\n"
            "  --title=NAME      window title\n"
            "  --password=PW     password (only sensible with --session-stdin)\n"
            "  --gw-host=HOST[:PORT] --gw-user=U --gw-domain=D --gw-password=P --gw-same-creds\n"
            "                    connect through an RD Gateway\n"
            "  --session-stdin   read all arguments, NUL-separated, from stdin (keeps\n"
            "                    passwords out of the process list)\n\n"
            "Any other argument is passed to FreeRDP (see xfreerdp --help).\n"
            "Keys: Ctrl+Alt+Enter toggles fullscreen.\n",
            argv0, argv0);
}

static bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

static int runSession(const char* argv0, const std::vector<std::string>& args, bool launched) {
    fastrdp::AppOptions opts;
    opts.launched = launched;
    std::vector<std::string> rdpArgs{argv0};

    for (const std::string& a : args) {
        if (a == "--vsync") opts.vsync = true;
        else if (a == "--no-h264") opts.h264 = false;
        else if (a == "--sw-decode") opts.hwDecode = false;
        else if (a == "--stats") opts.printStats = true;
        else if (a == "--scale=fit") opts.scale = fastrdp::ScaleMode::Fit;
        else if (a == "--scale=stretch") opts.scale = fastrdp::ScaleMode::Stretch;
        else if (a == "--scale=native") opts.scale = fastrdp::ScaleMode::Native;
        else if (startsWith(a, "--title=")) opts.title = a.substr(8);
        else if (startsWith(a, "--password=")) opts.password = a.substr(11);
        else if (startsWith(a, "--gw-host=")) {
            std::string hp = a.substr(10);
            const auto colon = hp.rfind(':');
            if (colon != std::string::npos && hp.find(']') == std::string::npos) {
                opts.gateway.port = uint32_t(std::strtoul(hp.c_str() + colon + 1, nullptr, 10));
                hp.resize(colon);
            }
            opts.gateway.host = hp;
        } else if (startsWith(a, "--gw-user=")) {
            opts.gateway.username = a.substr(10);
            opts.gateway.sameCredentials = false;
        } else if (startsWith(a, "--gw-domain=")) opts.gateway.domain = a.substr(12);
        else if (startsWith(a, "--gw-password=")) opts.gateway.password = a.substr(14);
        else if (a == "--gw-same-creds") opts.gateway.sameCredentials = true;
        else if (a == "--help" || a == "-h") {
            usage(argv0);
            return 0;
        } else {
            if (startsWith(a, "/size:") || startsWith(a, "/w:") || startsWith(a, "/h:"))
                opts.userSetSize = true;
            if (startsWith(a, "/network")) opts.userSetNetwork = true;
            if (a == "/f" || a == "+f") opts.fullscreen = true;
            rdpArgs.push_back(a);
        }
    }

    std::vector<char*> argvOut;
    for (auto& s : rdpArgs) argvOut.push_back(s.data());
    argvOut.push_back(nullptr);

    fastrdp::App app(opts);
    const int rc = app.run(int(rdpArgs.size()), argvOut.data());

    // Scrub the copies of any secrets we were handed.
    for (auto& s : rdpArgs) std::fill(s.begin(), s.end(), '\0');
    std::fill(opts.password.begin(), opts.password.end(), '\0');
    std::fill(opts.gateway.password.begin(), opts.gateway.password.end(), '\0');
    return rc;
}

int main(int argc, char** argv) {
    if (argc == 1) return fastrdp::runLauncher(argc, argv);

    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.size() == 1 && args[0] == "--session-stdin") {
        std::string all((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        args.clear();
        size_t start = 0;
        for (size_t i = 0; i <= all.size(); i++) {
            if (i == all.size() || all[i] == '\0') {
                if (i > start) args.emplace_back(all.substr(start, i - start));
                start = i + 1;
            }
        }
        std::fill(all.begin(), all.end(), '\0');
        return runSession(argv[0], args, true);
    }

    if (args[0] == "--help" || args[0] == "-h") {
        usage(argv[0]);
        return 0;
    }
    return runSession(argv[0], args, false);
}
