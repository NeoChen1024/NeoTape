#include "neotape/cli.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool cond, const std::string &msg) {
    if (!cond) {
        std::cerr << "test_cli: " << msg << "\n";
        std::exit(1);
    }
}

void test_locator_parse() {
    auto tape = neotape::parse_locator("tape:/dev/nst0");
    require(tape.kind == "tape", "tape locator kind");
    require(tape.locator == "/dev/nst0", "tape locator body");

    auto spool = neotape::parse_locator("spool:relative:path");
    require(spool.kind == "spool", "spool locator kind");
    require(spool.locator == "relative:path", "only first colon splits locator");
}

void test_locator_rejects_invalid() {
    const char *bad[] = {"", "tape", ":/dev/nst0", "spool:"};
    for (const char *text : bad) {
        bool threw = false;
        try {
            (void)neotape::parse_locator(text);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        require(threw, std::string("invalid locator rejected: ") + text);
    }
}

void test_control_policy_parse() {
    require(neotape::parse_control_policy("auto") ==
                neotape::ControlPolicy::auto_prompt,
            "auto control policy");
    require(neotape::parse_control_policy("none") ==
                neotape::ControlPolicy::none,
            "none control policy");

    bool threw = false;
    try {
        (void)neotape::parse_control_policy("tty");
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    require(threw, "invalid control policy rejected");
}

} // namespace

int main() {
    test_locator_parse();
    test_locator_rejects_invalid();
    test_control_policy_parse();
    return 0;
}
