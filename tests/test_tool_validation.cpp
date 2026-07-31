#include "agent/registry.h"
#include "common/config.h"
#include <iostream>

using nlohmann::json;
using namespace lar;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

int main() {
    Registry registry;
    register_tool_demo_job(registry);

    Config cfg;
    cfg.write_root.clear();
    register_tool_files(registry, cfg);

    const Tool* demo = registry.find("demo_job");
    expect("demo_job registered", demo != nullptr);
    if (demo) {
        expect("demo_job accepts minimum", demo->validate && demo->validate(json{{"seconds", 1}}).empty());
        expect("demo_job accepts maximum", demo->validate && demo->validate(json{{"seconds", 120}}).empty());
        expect("demo_job rejects zero", demo->validate && !demo->validate(json{{"seconds", 0}}).empty());
        expect("demo_job rejects negative", demo->validate && !demo->validate(json{{"seconds", -1}}).empty());
        expect("demo_job rejects above maximum", demo->validate && !demo->validate(json{{"seconds", 121}}).empty());
    }

    const Tool* write = registry.find("write_text_file");
    expect("write_text_file registered", write != nullptr);
    if (write) {
        expect("write accepts overwrite mode", write->validate &&
               write->validate(json{{"append", false}, {"overwrite", true}}).empty());
        expect("write accepts append mode", write->validate &&
               write->validate(json{{"append", true}, {"overwrite", false}}).empty());
        expect("write rejects conflicting modes", write->validate &&
               !write->validate(json{{"append", true}, {"overwrite", true}}).empty());
    }

    if (failures) return 1;
    std::cout << "all tool-validation tests passed\n";
    return 0;
}
