#pragma once
#include <vector>
#include <string>
#include "state_store.hpp"
#include "image_manager.hpp"

extern std::vector<VolumeMount> volumes;
extern std::vector<std::string> commands;
extern std::string image_name;
extern std::string container_id;
extern std::string container_name;
extern pid_t container_pid;
extern bool vfs;
extern bool no_remove;

class CommandLineHandler {
public:
    static void run(StateStore& store, const std::vector<std::string>& cmds);
    static void attach(StateStore& store, const std::vector<std::string>& cmds);
    static void ps(StateStore& store, const std::vector<std::string>& cmds);
    static void rm(StateStore& store, const std::vector<std::string>& cmds);
    static void image(StateStore& store, ImageManager& img_manager, const std::vector<std::string>& cmds);
    static void volume(StateStore& store, const std::vector<std::string>& cmds);
    static void network(StateStore& store, const std::vector<std::string>& cmds);
    static void create(StateStore& store, const std::vector<std::string>& cmds);
    static void pull(StateStore& store, const std::vector<std::string>& cmds);
    static void start(StateStore& store, const std::vector<std::string>& cmds);
    static void stop(StateStore& store, const std::vector<std::string>& cmds);
    static void vfs(StateStore& store, const std::vector<std::string>& cmds);
};
