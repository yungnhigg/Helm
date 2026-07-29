// Stream filter tests. Fed byte-by-byte and in chunks, since a real token
// stream splits the envelope at arbitrary points.
#include "agent/stream_filter.h"
#include <iostream>
#include <string>
#include <vector>

using namespace lar;

static int failures = 0;

static void check(const std::string& what, const std::string& got, const std::string& want) {
    if (got == want) return;
    std::cerr << "FAIL " << what << "\n  got:  [" << got << "]\n  want: [" << want << "]\n";
    ++failures;
}

static void check_kind(const std::string& what, StreamFilter::Kind got, StreamFilter::Kind want) {
    if (got == want) return;
    std::cerr << "FAIL " << what << " (kind)\n";
    ++failures;
}

// Feed one character at a time — the worst case for a prefix matcher.
static FilterOut drip(const std::string& raw, StreamFilter& f) {
    FilterOut acc;
    for (char c : raw) {
        FilterOut o = f.feed(std::string(1, c));
        acc.content  += o.content;
        acc.thinking += o.thinking;
        acc.note     += o.note;
    }
    return acc;
}

// Feed in fixed-size chunks.
static FilterOut chunked(const std::string& raw, size_t n, StreamFilter& f) {
    FilterOut acc;
    for (size_t i = 0; i < raw.size(); i += n) {
        FilterOut o = f.feed(raw.substr(i, n));
        acc.content  += o.content;
        acc.thinking += o.thinking;
        acc.note     += o.note;
    }
    return acc;
}

int main() {
    { // plain reply
        StreamFilter f;
        auto o = drip(R"({"type":"reply","content":"Hello there."})", f);
        check("plain reply content", o.content, "Hello there.");
        check("plain reply thinking empty", o.thinking, "");
        check_kind("plain reply", f.kind(), StreamFilter::Kind::Reply);
    }
    { // reply with thinking
        StreamFilter f;
        auto o = drip(R"({"type":"reply","thinking":"weighing options","content":"Answer."})", f);
        check("thinking routed", o.thinking, "weighing options");
        check("content after thinking", o.content, "Answer.");
    }
    { // escapes in content
        StreamFilter f;
        auto o = drip(R"({"type":"reply","content":"line1\nline2\t\"quoted\" back\\slash"})", f);
        check("escapes", o.content, "line1\nline2\t\"quoted\" back\\slash");
    }
    { // unicode escape + surrogate pair
        StreamFilter f;
        auto o = drip(R"({"type":"reply","content":"\u00e9 \ud83d\ude00"})", f);
        check("unicode", o.content, "\u00e9 \U0001F600");
    }
    { // lone high surrogate degrades to replacement char
        StreamFilter f;
        auto o = drip(R"({"type":"reply","content":"\ud800x"})", f);
        check("lone surrogate", o.content, "\uFFFDx");
    }
    { // tool call emits nothing on content
        StreamFilter f;
        auto o = drip(R"({"type":"tool_call","name":"get_time","arguments":{}})", f);
        check("tool call silent", o.content, "");
        check_kind("tool call", f.kind(), StreamFilter::Kind::ToolCall);
    }
    { // tool call with a note
        StreamFilter f;
        auto o = drip(R"({"type":"tool_call","note":"Trying a narrower search.","name":"search_web","arguments":{"query":"x","max_results":3}})", f);
        check("note routed", o.note, "Trying a narrower search.");
        check("note leaves content clean", o.content, "");
        check_kind("tool call w/ note", f.kind(), StreamFilter::Kind::ToolCall);
    }
    { // tool call with thinking AND note
        StreamFilter f;
        auto o = drip(R"({"type":"tool_call","thinking":"first attempt was too broad","note":"Narrowing.","name":"search_web","arguments":{"query":"x","max_results":3}})", f);
        check("both: thinking", o.thinking, "first attempt was too broad");
        check("both: note", o.note, "Narrowing.");
        check("both: content", o.content, "");
    }
    { // no grammar / model ignored it: passthrough
        StreamFilter f;
        auto o = drip("I am just talking normally.", f);
        check("raw passthrough", o.content, "I am just talking normally.");
        check_kind("raw", f.kind(), StreamFilter::Kind::Raw);
    }
    { // chunk boundaries must not matter
        const std::string raw = R"({"type":"reply","thinking":"abc","content":"Hello \u00e9 world"})";
        for (size_t n : {1u, 2u, 3u, 5u, 7u, 11u, 64u}) {
            StreamFilter f;
            auto o = chunked(raw, n, f);
            check("chunk size " + std::to_string(n) + " content", o.content, "Hello \u00e9 world");
            check("chunk size " + std::to_string(n) + " thinking", o.thinking, "abc");
        }
    }
    { // empty thinking string is legal and yields nothing
        StreamFilter f;
        auto o = drip(R"({"type":"reply","thinking":"","content":"hi"})", f);
        check("empty thinking", o.thinking, "");
        check("content with empty thinking", o.content, "hi");
    }

    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "all stream filter tests passed\n";
    return 0;
}
