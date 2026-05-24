#include <iostream>

namespace {

int stub(const char *name) {
    std::cerr << "neotape: subcommand not wired yet: " << name << "\n";
    return 1;
}

} // namespace

int neotape_write_main(int, char **) { return stub("write"); }
int neotape_read_main(int, char **) { return stub("read"); }
int neotape_backup_main(int, char **) { return stub("backup"); }
int neotape_restore_main(int, char **) { return stub("restore"); }
int neotape_list_main(int, char **) { return stub("list"); }
