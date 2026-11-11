// The log of this run and of the two before it: older ones go, nothing else moves.

#include "pc/log.h"
#include "pc/port.h"
#include "test.h"

using namespace xport::pc;

int main() {
    TemporaryFolder folder(XPORT_NAME "_test_log");
    const auto log = folder.path / XPORT_NAME ".log", one = folder.path / XPORT_NAME ".1.log", two = folder.path / XPORT_NAME ".2.log";

    rotate_log(log);  // the first start: nothing to move
    CHECK(!std::filesystem::exists(log) && !std::filesystem::exists(one) && !std::filesystem::exists(two));

    write_file(log, std::string("first"));
    rotate_log(log);
    CHECK(!std::filesystem::exists(log));
    CHECK(read_file(one) == "first");
    CHECK(!std::filesystem::exists(two));

    write_file(log, std::string("second"));
    rotate_log(log);
    CHECK(read_file(one) == "second");
    CHECK(read_file(two) == "first");

    write_file(log, std::string("third"));
    rotate_log(log);
    CHECK(read_file(one) == "third");
    CHECK(read_file(two) == "second");  // the first is gone
    CHECK(std::distance(std::filesystem::directory_iterator(folder.path), std::filesystem::directory_iterator()) == 2);

    return test_result();
}
