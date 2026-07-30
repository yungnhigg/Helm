#include "agent/run_guard.h"
#include <iostream>
#include <string>
#include <vector>

using nlohmann::json;
using namespace lar;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

int main() {
    std::vector<std::string> seen;

    const auto write_a = canonical_tool_call_signature(
        "write_text_file", json{{"path", "C:/work/a.txt"}, {"content", "one"}});
    const auto write_b = canonical_tool_call_signature(
        "write_text_file", json{{"path", "C:/work/a.txt"}, {"content", "two"}});

    expect("first args-only call is accepted", remember_tool_call(seen, write_a));
    expect("exact args-only repeat is refused", !remember_tool_call(seen, write_a));
    expect("different write content is accepted", remember_tool_call(seen, write_b));

    const auto url_a = canonical_tool_call_signature(
        "fetch_web_page", json{{"url", "https://example.com"}, {"max_chars", 1000}});
    const auto url_b = canonical_tool_call_signature(
        "fetch_web_page", json{{"url", "https://example.com"}, {"max_chars", 2000}});
    expect("first URL call is accepted", remember_tool_call(seen, url_a));
    expect("different URL options remain valid", remember_tool_call(seen, url_b));
    expect("exact URL repeat is refused", !remember_tool_call(seen, url_b));

    std::vector<std::string> bounded;
    expect("bounded first", remember_tool_call(bounded, "a", 2));
    expect("bounded second", remember_tool_call(bounded, "b", 2));
    expect("bounded third", remember_tool_call(bounded, "c", 2));
    expect("bounded storage remains capped", bounded.size() <= 2);

    if (failures) return 1;
    std::cout << "all run-guard tests passed\n";
    return 0;
}
