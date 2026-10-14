#include "../include/image_manager.hpp"
#include "../include/state_store.hpp"
#include "../include/utils.hpp"
#include "../include/command_line_handler.hpp"
#include <cstdlib>
#include <functional>
#include <string>
#include <iostream>
#include <map>
#include <vector>

using CommandHandler = std::function<void(const std::vector<std::string>&)>;

int main(int argc, char* argv[]) {
    try {
        std::string base_dir{ Utils::get_base_dir() };
        StateStore store{ base_dir };

        ImageManager img_manager(store);
        CommandLineHandler handler(store, img_manager);

        if (argc < 2) {
            Utils::print_usage();
            return EXIT_FAILURE;
        }

        std::map<std::string, CommandHandler> commands{};
        commands["run"]     = [&handler](const std::vector<std::string>& args) { handler.run(args); };
        commands["attach"]  = [&handler](const std::vector<std::string>& args) { handler.attach(args); };
        commands["ps"]      = [&handler](const std::vector<std::string>& args) { handler.ps(args); };
        commands["rm"]      = [&handler](const std::vector<std::string>& args) { handler.rm(args); };
        commands["start"]   = [&handler](const std::vector<std::string>& args) { handler.start(args); };
        commands["stop"]    = [&handler](const std::vector<std::string>& args) { handler.stop(args); };
        commands["image"]   = [&handler](const std::vector<std::string>& args) { handler.image(args); };
        commands["volume"]  = [&handler](const std::vector<std::string>& args) { handler.volume(args); };
        commands["network"] = [&handler](const std::vector<std::string>& args) { handler.network(args); };
        commands["create"]  = [&handler](const std::vector<std::string>& args) { handler.create(args); };
        commands["pull"]    = [&handler](const std::vector<std::string>& args) { handler.pull(args); };
        commands["vfs"]     = [&handler](const std::vector<std::string>& args) { handler.vfs(args); };
        commands["help"]    = [](const std::vector<std::string>&) { Utils::print_usage(); };

        std::vector<std::string> cmds;
        for (int i = 2; i < argc; ++i) {
            cmds.emplace_back(argv[i]);
        }

        auto it = commands.find(argv[1]);
        if (it == commands.end()) {
            Utils::print_usage();
            return EXIT_FAILURE;
        } else {
            it->second(cmds);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
