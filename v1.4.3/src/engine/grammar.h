#pragma once
// Builds the GBNF grammar that constrains every generation.
//
// The model can emit exactly two shapes and nothing else:
//
//   {"type":"reply","thinking":"...","content":"..."}
//   {"type":"tool_call","thinking":"...","note":"...","name":"<registered>","arguments":{...}}
//
// "thinking" and "note" are optional and always precede the fields they
// annotate, so the stream filter can route them to their own channels as they
// arrive rather than waiting for the envelope to close.
//
// Why "note" exists: without it a reply was the only way for the model to say
// anything, and a reply ends the turn. A model that wanted to narrate before
// acting had to stop in order to do it ("let me try a more specific search" —
// then silence, waiting for the user). Carrying narration on the tool call
// makes explain-and-act one atomic move.
//
// Byte-exact, no whitespace anywhere — the stream filter and the parser both
// depend on that.
#include <string>
#include <vector>

namespace lar {

enum class ParamType { String, Number, Integer, Boolean };

struct GrammarParam {
    std::string name;
    ParamType   type;
};

struct GrammarTool {
    std::string name;
    std::vector<GrammarParam> params; // fixed order, all required (v1 constraint)
};

struct GrammarOptions {
    bool thinking = true; // allow a leading "thinking" field
    bool notes    = true; // allow "note" on tool calls
};

// Returns a complete GBNF grammar with root rule "root".
std::string build_agent_grammar(const std::vector<GrammarTool>& tools,
                                const GrammarOptions& opt = {});

} // namespace lar
