#include "engine/grammar.h"
#include <sstream>

namespace lar {

// Escape a literal for inclusion inside a GBNF double-quoted terminal.
static std::string lit(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string build_agent_grammar(const std::vector<GrammarTool>& tools,
                                const GrammarOptions& opt) {
    std::ostringstream g;

    // Shared JSON primitives
    g << "jstring ::= \"\\\"\" jchar* \"\\\"\"\n"
         "jchar ::= [^\"\\\\\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" hex hex hex hex)\n"
         "hex ::= [0-9a-fA-F]\n"
         "jnumber ::= \"-\"? (\"0\" | [1-9] [0-9]*) (\".\" [0-9]+)? ([eE] [-+]? [0-9]+)?\n"
         "jint ::= \"-\"? (\"0\" | [1-9] [0-9]*)\n"
         "jbool ::= \"true\" | \"false\"\n";

    // Optional annotation fields. Each carries its own trailing comma so the
    // field that follows is always written the same way.
    if (opt.thinking)
        g << "think ::= \"" << lit(R"("thinking":)") << "\" jstring \",\"\n";
    if (opt.notes)
        g << "note ::= \"" << lit(R"("note":)") << "\" jstring \",\"\n";

    const std::string think_opt = opt.thinking ? " think?" : "";
    const std::string note_opt  = opt.notes    ? " note?"  : "";

    // reply shape
    g << "reply ::= \"" << lit(R"({"type":"reply",)") << "\""
      << think_opt
      << " \"" << lit(R"("content":)") << "\" jstring \"}\"\n";

    if (tools.empty()) {
        g << "root ::= reply\n";
        return g.str();
    }

    // one rule per tool
    for (size_t t = 0; t < tools.size(); ++t) {
        const auto& tool = tools[t];
        g << "call" << t << " ::= \"" << lit(R"({"type":"tool_call",)") << "\""
          << think_opt << note_opt
          << " \"" << lit(R"("name":")") << lit(tool.name) << lit(R"(","arguments":{)") << "\"";
        for (size_t p = 0; p < tool.params.size(); ++p) {
            const auto& par = tool.params[p];
            g << " \"" << (p ? "," : "") << lit("\"" + par.name + "\":") << "\" ";
            switch (par.type) {
                case ParamType::String:  g << "jstring"; break;
                case ParamType::Number:  g << "jnumber"; break;
                case ParamType::Integer: g << "jint";    break;
                case ParamType::Boolean: g << "jbool";   break;
            }
        }
        g << " \"}}\"\n";
    }

    g << "toolcall ::=";
    for (size_t t = 0; t < tools.size(); ++t) g << (t ? " |" : "") << " call" << t;
    g << "\n";

    g << "root ::= reply | toolcall\n";
    return g.str();
}

} // namespace lar
