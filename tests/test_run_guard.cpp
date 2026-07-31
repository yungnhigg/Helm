#include "agent/run_guard.h"
#include <iostream>
#include <string>

using nlohmann::json;
using namespace lar;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

int main() {
    const auto query_a = canonical_tool_call_signature(
        "search_web", json{{"query", "  Qwen   CUDA "}, {"max_results", 5}});
    const auto query_b = canonical_tool_call_signature(
        "SEARCH_WEB", json{{"query", "qwen cuda"}, {"max_results", 5}});
    expect("query whitespace and case normalize", query_a == query_b);

    const auto write_a = canonical_tool_call_signature(
        "write_text_file", json{{"path", "C:\\Work\\A.txt"}, {"content", "one"}, {"overwrite", true}});
    const auto write_b = canonical_tool_call_signature(
        "write_text_file", json{{"path", "c:/work/a.txt"}, {"content", "two"}, {"overwrite", true}});
    expect("different write content remains distinct", write_a != write_b);

    const auto fetch_a = canonical_tool_call_signature(
        "fetch_web_page", json{{"url", "HTTPS://Example.com/docs/#top"}, {"max_chars", 1000}});
    const auto fetch_b = canonical_tool_call_signature(
        "fetch_web_page", json{{"url", "https://example.com/docs"}, {"max_chars", 2000}});
    expect("different fetch ranges remain distinct", fetch_a != fetch_b);
    expect("same URL has same resource key",
        canonical_tool_resource_key("fetch_web_page", json{{"url", "HTTPS://Example.com/docs/#top"}}) ==
        canonical_tool_resource_key("fetch_web_page", json{{"url", "https://example.com/docs"}}));
    expect("archive seen-list paths normalize",
        canonical_tool_resource_key("archive_seen", json{{"file", "C:\\Work\\seen.txt"}}) ==
        canonical_tool_resource_key("ARCHIVE_SEEN", json{{"file", "c:/work/seen.txt"}}));

    ProgressWatchdog exact;
    auto first = exact.before_call("search_web", json{{"query", "helm"}});
    exact.after_result("search_web", json{{"query", "helm"}}, "useful result");
    auto repeat1 = exact.before_call("search_web", json{{"query", " HELM "}});
    auto repeat2 = exact.before_call("search_web", json{{"query", "helm"}});
    auto repeat3 = exact.before_call("search_web", json{{"query", "helm"}});
    expect("first call accepted", first.allow);
    expect("exact repeat refused", !repeat1.allow && !repeat1.abort_run);
    expect("second refusal still recoverable", !repeat2.allow && !repeat2.abort_run);
    expect("third repeated refusal aborts non-progress loop", !repeat3.allow && repeat3.abort_run);

    ProgressWatchdog workflow;
    json read_args{{"path", "C:\\work\\file.txt"}, {"offset", 0}};
    expect("workflow first read accepted", workflow.before_call("read_text_file", read_args).allow);
    workflow.after_result("read_text_file", read_args, "old content");
    json write_args{{"path", "C:\\work\\file.txt"}, {"content", "new content"},
                    {"overwrite", true}, {"append", false}};
    expect("workflow write accepted", workflow.before_call("write_text_file", write_args).allow);
    workflow.after_result("write_text_file", write_args, "wrote C:/work/file.txt");
    expect("read may repeat after intervening material progress",
           workflow.before_call("read_text_file", read_args).allow);

    ProgressWatchdog narration;
    json repeated_write{{"path", "C:\\work\\same.txt"}, {"content", "same"},
                        {"overwrite", true}, {"append", false}};
    expect("side-effect call starts", narration.before_call("write_text_file", repeated_write).allow);
    narration.after_result("write_text_file", repeated_write, "wrote C:/work/same.txt");
    narration.on_reply_progress();
    expect("reply narration does not unlock identical side effect",
           !narration.before_call("write_text_file", repeated_write).allow);

    ProgressWatchdog random_tools;
    expect("first dice roll accepted", random_tools.before_call("roll_dice", json{{"sides", 6}}).allow);
    random_tools.after_result("roll_dice", json{{"sides", 6}}, "4");
    expect("nondeterministic dice repeat remains valid",
           random_tools.before_call("roll_dice", json{{"sides", 6}}).allow);


    ProgressWatchdog transient;
    auto failed_first = transient.before_call("fetch_web_page", json{{"url", "https://example.com"}});
    transient.after_result("fetch_web_page", json{{"url", "https://example.com"}}, "error: temporary network failure");
    auto failed_retry = transient.before_call("fetch_web_page", json{{"url", "https://example.com"}});
    transient.after_result("fetch_web_page", json{{"url", "https://example.com"}}, "error: temporary network failure");
    auto failed_third = transient.before_call("fetch_web_page", json{{"url", "https://example.com"}});
    expect("first failed call accepted", failed_first.allow);
    expect("one exact retry allowed after failure", failed_retry.allow);
    expect("second failed retry blocked", !failed_third.allow);

    ProgressWatchdog persisted;
    const std::string prior_resource = canonical_tool_resource_key(
        "search_web", json{{"query", "persisted topic"}, {"max_results", 5}});
    persisted.seed_resource_observation(prior_resource,
        stable_text_fingerprint("same persisted result"), false);
    json persisted_args{{"query", "persisted topic"}, {"max_results", 10}};
    expect("one persistent resource refresh is allowed", persisted.before_call("search_web", persisted_args).allow);
    auto persisted_result = persisted.after_result("search_web", persisted_args, "same persisted result");
    expect("unchanged persistent refresh marks resource stalled", persisted_result.resource_stalled);
    expect("second unchanged persistent refresh is blocked",
           !persisted.before_call("search_web", json{{"query", "persisted topic"}, {"max_results", 20}}).allow);

    ProgressWatchdog stagnant;
    for (int i = 1; i <= 3; ++i) {
        json args{{"query", "same topic"}, {"max_results", i}};
        expect("variant call accepted before stall", stagnant.before_call("search_web", args).allow);
        stagnant.after_result("search_web", args, "identical result");
    }
    auto stalled = stagnant.before_call("search_web", json{{"query", "same topic"}, {"max_results", 4}});
    expect("stalled resource blocked without a call budget", !stalled.allow);

    ProgressWatchdog plans;
    expect("first plan-only reply continues", !plans.on_stalling_reply("Let me check:").abort_run);
    plans.on_stalling_reply("Now let me inspect:");
    plans.on_stalling_reply("I'll do that:");
    expect("fourth plan-only reply stops", plans.on_stalling_reply("I will start:").abort_run);

    expect("fingerprint stable", stable_text_fingerprint("abc") == stable_text_fingerprint("abc"));
    expect("fingerprint changes", stable_text_fingerprint("abc") != stable_text_fingerprint("abd"));

    if (failures) return 1;
    std::cout << "all run-guard tests passed\n";
    return 0;
}
