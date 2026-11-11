// The launcher's update check without the network: version order and which links from GitHub's answer are used.
#include <string>

#include "pc/update_check.h"
#include "test.h"

using namespace xport::pc;

int main() {
    // Versions compare number by number, with or without the tag's "v".
    CHECK(version_numbers("v0.3.10") > version_numbers("0.3.9"));
    CHECK(version_numbers("v0.3.5") == version_numbers("0.3.5"));
    CHECK(version_numbers("0.4.0") > version_numbers("0.3.99"));
    CHECK(!(version_numbers("0.3.5") > version_numbers("0.3.5")));
    CHECK(version_numbers("").empty());

    const char* repository = "owner/someport";
    const std::string downloads = "https://github.com/owner/someport/releases/download/";
    CHECK(release_link(downloads + "v0.3.5/someport-0.3.5-windows-x86-setup.exe", repository, "download/"));
    CHECK(release_link(downloads + "v0.3.5/SHA256SUMS", repository, "download/"));
    CHECK(release_link("https://github.com/owner/someport/releases/tag/v0.3.5", repository, "tag/"));

    // Anything else is refused: another repository or host, another kind of page, a way out of the folder or a
    // name that would not be a plain file in %TEMP%.
    CHECK(!release_link("https://github.com/someone/someport/releases/download/v0.3.5/x.exe", repository, "download/"));
    CHECK(!release_link("https://example.com/owner/someport/releases/download/v1/x.exe", repository, "download/"));
    CHECK(!release_link("http://github.com/owner/someport/releases/download/v1/x.exe", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/x.exe", repository, "tag/"));
    CHECK(!release_link(downloads + "v0.3.5/../../../evil/x.exe", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/a\\..\\b.exe", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/x.exe?y=1", repository, "download/"));
    CHECK(!release_link(downloads + "v0.3.5/", repository, "download/"));
    CHECK(!release_link(downloads, repository, "download/"));

    // A release's notes as GitHub's answer holds them, and the points the launcher shows.
    const std::string answer =
        R"({"tag_name":"v0.3.7","body":"## 0.3.7\r\n\r\n### New\r\n\r\n- **Your own music** on the radio (#17). More here.\r\n)"
        R"(  Choose it in the game.\r\n- A [link](https://x.y/z) and `code`, version 0.3.7. Then more.\r\n  - a sub-point\r\n)"
        R"(\r\n### Fixed\r\n\r\n* Café \"quoted\" \\ slash\/ 🚗\r\n","draft":false})";
    const std::string notes = json_text(answer, "body");
    CHECK(notes.find("## 0.3.7\n\n### New") == 0);
    CHECK(json_text(answer, "tag_name") == "v0.3.7" && json_text(answer, "missing").empty());
    const std::vector<std::string> points = release_points(notes, 10);
    CHECK(points.size() == 3);
    CHECK(points[0] == "Your own music on the radio (#17).");
    CHECK(points[1] == "A link and code, version 0.3.7.");
    CHECK(points[2] == "Caf\xC3\xA9 \"quoted\" \\ slash/ \xF0\x9F\x9A\x97");
    CHECK(release_points(notes, 1).size() == 1 && release_points("no list here", 5).empty());

    return test_result();
}
