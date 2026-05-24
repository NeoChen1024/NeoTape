#include "neotape/commands.hpp"

#include <exception>
#include <format>
#include <iostream>
#include <string_view>

namespace {

void usage(const char *prog) {
    std::cerr << std::format("usage: {} <subcommand> [options]\n"
                             "\n"
                             "subcommands:\n"
                             "  init      initialize a tape or spool medium\n"
                             "  list      list archive instances\n"
                             "  plan      create PAX plan metadata\n"
                             "  backup    write a PAX-profile archive\n"
                             "  restore   read a PAX-profile archive\n"
                             "  write     write a raw-profile archive\n"
                             "  read      read a raw-profile archive\n",
                             prog);
}

int dispatch(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::string_view cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage(argv[0]);
        return 0;
    }

    char **sub_argv = argv + 1;
    int sub_argc = argc - 1;

    if (cmd == "init")
        return neotape_init_main(sub_argc, sub_argv);
    if (cmd == "plan")
        return neotape_plan_main(sub_argc, sub_argv);
    if (cmd == "write")
        return neotape_write_main(sub_argc, sub_argv);
    if (cmd == "read")
        return neotape_read_main(sub_argc, sub_argv);
    if (cmd == "backup")
        return neotape_backup_main(sub_argc, sub_argv);
    if (cmd == "restore")
        return neotape_restore_main(sub_argc, sub_argv);
    if (cmd == "list")
        return neotape_list_main(sub_argc, sub_argv);

    std::cerr << std::format("{}: unknown subcommand: {}\n", argv[0], cmd);
    usage(argv[0]);
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    try {
        return dispatch(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << std::format("neotape: {}\n", e.what());
        return 1;
    }
}
