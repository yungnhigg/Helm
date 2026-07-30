// Registry filtering is the prompt-side half of per-agent permissions. These
// checks pin the invariant that a denied tool is absent from JSON docs, Harmony
// declarations, and grammar specs generated for that agent.
#include "agent/registry.h"
#include <iostream>
#include <string>
#include <utility>

using namespace lar;

static int failures = 0;

static void expect(const std::string& what, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << what << "\n";
    ++failures;
}

static bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int main() {
    Registry registry;

    Tool read;
    read.name = "read_text_file";
    read.description = "Read a local text file.";
    read.params = {{"path", ParamType::String, "File path."}};
    registry.add(std::move(read));

    Tool write;
    write.name = "write_text_file";
    write.description = "Write a local text file.";
    write.params = {{"path", ParamType::String, "File path."},
                    {"content", ParamType::String, "New contents."}};
    registry.add(std::move(write));

    const Registry::AllowSet read_only = {"read_text_file"};

    const auto all_specs = registry.grammar_specs();
    const auto filtered_specs = registry.grammar_specs(read_only);
    expect("unfiltered specs retain both tools", all_specs.size() == 2);
    expect("filtered specs retain one tool", filtered_specs.size() == 1);
    expect("filtered spec is read tool", filtered_specs.size() == 1 &&
           filtered_specs.front().name == "read_text_file");

    const std::string docs = registry.prompt_docs(read_only);
    expect("JSON docs include allowed tool", has(docs, "### read_text_file"));
    expect("JSON docs exclude denied tool", !has(docs, "### write_text_file"));

    const std::string harmony = registry.harmony_docs(read_only);
    expect("Harmony includes allowed tool", has(harmony, "type read_text_file"));
    expect("Harmony excludes denied tool", !has(harmony, "type write_text_file"));

    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "all registry tests passed\n";
    return 0;
}
