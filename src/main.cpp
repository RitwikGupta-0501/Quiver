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

using CommandHandler = std::function<void(StateStore&, ImageManager&, const std::vector<std::string>&)>;

int main(int argc, char* argv[]) {
    try {
        std::string base_dir{ Utils::get_base_dir() };
        StateStore store{ base_dir };

        ImageManager img_manager(store);

        if (argc < 2) {
            Utils::print_usage();
            return EXIT_FAILURE;
        }

        std::map<std::string, CommandHandler> commands{};
        commands["run"]    = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::run(store, args); };
        commands["attach"] = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::attach(store, args); };
        commands["ps"]     = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::ps(store, args); };
        commands["rm"]     = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::rm(store, args); };
        commands["start"]  = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::start(store, args); };
        commands["stop"]   = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::stop(store, args); };
        commands["image"]  = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::image(store, img, args); };
        commands["volume"] = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::volume(store, args); };
        commands["network"] = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::network(store, args); };
        commands["create"] = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::create(store, args); };
        commands["pull"]   = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::pull(store, args); };
        commands["vfs"]    = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ CommandLineHandler::vfs(store, args); };
        commands["help"]   = [](StateStore& store[[maybe_unused]],
                                ImageManager& img[[maybe_unused]],
                                const std::vector<std::string>& args[[maybe_unused]]){ Utils::print_usage(); };

        std::vector<std::string> cmds;
        for (int i = 2; i < argc; ++i) {
            cmds.emplace_back(argv[i]);
        }

        auto it{commands.find(argv[1])};
        if (it == commands.end()) {
            Utils::print_usage();
            return EXIT_FAILURE;
        } else {
            it->second(store, img_manager, cmds);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
