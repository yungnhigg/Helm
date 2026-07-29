#pragma once
// Live routing for GPT-OSS Harmony output.
//
// Harmony is not the JSON envelope the grammar enforces for every other model,
// so StreamFilter cannot read it. Without something in its place the window
// stays frozen for the whole generation and the thinking pane never fills —
// which is what suppressing the stream entirely costs.
//
// Harmony frames look like:
//
//   <|channel|>analysis<|message|>...private reasoning...<|end|>
//   <|channel|>commentary to=functions.NAME <|constrain|>json<|message|>{...}<|call|>
//   <|channel|>final<|message|>...the answer...<|return|>
//
// Routing: analysis -> thinking pane, final -> visible reply, commentary
// without a recipient -> note. Commentary addressed to a function is the tool
// call itself and is suppressed; loop.cpp announces the call once the whole
// response parses.
//
// Implementation is a re-scan rather than a resumable state machine: the raw
// text is reparsed on each chunk and only the growth of each channel is
// emitted. Every channel accumulator is append-only, so the delta is always a
// suffix. Generations are a few kilobytes, so the cost is irrelevant next to
// the cost of a subtle bug in a hand-rolled incremental parser.
#include "agent/stream_filter.h" // FilterOut
#include <string>

namespace lar {

class HarmonyStreamFilter {
public:
    FilterOut feed(const std::string& chunk) {
        raw_ += chunk;
        std::string thinking, content, note;
        parse(thinking, content, note);

        FilterOut out;
        out.thinking = delta(thinking, sent_thinking_);
        out.content  = delta(content,  sent_content_);
        out.note     = delta(note,     sent_note_);
        return out;
    }

private:
    static std::string delta(const std::string& total, std::string& already_sent) {
        // Defensive: if a reparse ever shrinks a channel, resynchronise rather
        // than slicing out of range.
        if (total.size() <= already_sent.size()) { already_sent = total; return {}; }
        std::string piece = total.substr(already_sent.size());
        already_sent = total;
        return piece;
    }

    void parse(std::string& thinking, std::string& content, std::string& note) const {
        static const std::string CHANNEL = "<|channel|>";
        static const std::string MESSAGE = "<|message|>";

        size_t pos = 0;
        while (true) {
            const size_t chan = raw_.find(CHANNEL, pos);
            if (chan == std::string::npos) return;
            const size_t header_begin = chan + CHANNEL.size();
            const size_t msg = raw_.find(MESSAGE, header_begin);
            if (msg == std::string::npos) return; // header still arriving

            const std::string header = raw_.substr(header_begin, msg - header_begin);
            const size_t body_begin = msg + MESSAGE.size();

            // Body runs to the next control marker, or to the end of what has
            // arrived so far. A trailing lone '<' is withheld: it may be the
            // first byte of "<|".
            size_t body_end = raw_.find("<|", body_begin);
            bool complete = body_end != std::string::npos;
            if (!complete) {
                body_end = raw_.size();
                if (body_end > body_begin && raw_[body_end - 1] == '<') --body_end;
            }
            const std::string body = raw_.substr(body_begin, body_end - body_begin);

            if (header.starts_with("analysis")) thinking += body;
            else if (header.starts_with("final")) content += body;
            else if (header.starts_with("commentary")) {
                // A commentary message addressed to a function carries the tool
                // arguments. Narration on the same channel has no recipient.
                if (header.find("to=") == std::string::npos) note += body;
            }

            if (!complete) return;
            pos = body_end;
        }
    }

    std::string raw_;
    std::string sent_thinking_;
    std::string sent_content_;
    std::string sent_note_;
};

} // namespace lar
