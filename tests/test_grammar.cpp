// Grammar builder tests. The stream filter and the JSON parser both assume the
// envelope is byte-exact, so these pin the exact shapes rather than just
// checking that a rule exists.
#include "engine/grammar.h"
#include <iostream>
#include <string>
#include <vector>

using namespace lar;

static int failures = 0;

static void expect(const std::string& what, bool cond) {
    if (cond) return;
    std::cerr << "FAIL " << what << "\n";
    ++failures;
}

static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // ---- chat mode: reply only
    {
        const std::string chat = build_agent_grammar({});
        expect("chat roots at reply", has(chat, "root ::= reply\n"));
        expect("chat has no toolcall", !has(chat, "toolcall"));
        expect("jint is not a bare digit run", has(chat, "jint ::= \"-\"? (\"0\" | [1-9] [0-9]*)"));
        expect("thinking rule present by default", has(chat, "think ::= \"\\\"thinking\\\":\" jstring \",\""));
        expect("reply accepts optional thinking", has(chat, "reply ::= \"{\\\"type\\\":\\\"reply\\\",\" think? \"\\\"content\\\":\" jstring \"}\""));
    }

    // ---- agent mode: reply | toolcall, with note
    {
        GrammarTool tool;
        tool.name = "write_file";
        tool.params = {{"path", ParamType::String}, {"overwrite", ParamType::Boolean}};
        const std::string agent = build_agent_grammar({tool});

        expect("tool name embedded", has(agent, "write_file"));
        expect("agent root", has(agent, "root ::= reply | toolcall\n"));
        expect("note rule present", has(agent, "note ::= \"\\\"note\\\":\" jstring \",\""));
        expect("call allows thinking then note, in that order",
               has(agent, "call0 ::= \"{\\\"type\\\":\\\"tool_call\\\",\" think? note? \"\\\"name\\\":\\\"write_file\\\""));
        expect("boolean param maps to jbool", has(agent, "\",\\\"overwrite\\\":\" jbool"));
        expect("string param maps to jstring", has(agent, "\"\\\"path\\\":\" jstring"));
    }

    // ---- options off: neither annotation is offered to the model
    {
        GrammarTool tool;
        tool.name = "get_time";
        GrammarOptions off{false, false};
        const std::string bare = build_agent_grammar({tool}, off);
        expect("no think rule when disabled", !has(bare, "think ::="));
        expect("no note rule when disabled", !has(bare, "note ::="));
        expect("no optional markers when disabled", !has(bare, "think?") && !has(bare, "note?"));
        expect("reply still well formed", has(bare, "reply ::= \"{\\\"type\\\":\\\"reply\\\",\" \"\\\"content\\\":\" jstring \"}\""));
        expect("zero-param tool closes cleanly", has(bare, "\\\"arguments\\\":{\" \"}}\""));
    }

    // ---- thinking on, notes off
    {
        GrammarTool tool;
        tool.name = "x";
        GrammarOptions mixed{true, false};
        const std::string g = build_agent_grammar({tool}, mixed);
        expect("mixed: think present", has(g, "think ::="));
        expect("mixed: note absent", !has(g, "note ::="));
        expect("mixed: call takes think only", has(g, "call0 ::= \"{\\\"type\\\":\\\"tool_call\\\",\" think? \"\\\"name\\\""));
    }

    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "all grammar tests passed\n";
    return 0;
}
