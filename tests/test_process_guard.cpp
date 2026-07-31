#include "tools/process_guard.h"
#include <iostream>
#include <string>

using namespace lar;

static int failures = 0;
static void expect(const char* name, bool condition) {
    if (condition) return;
    std::cerr << "FAIL " << name << "\n";
    ++failures;
}

int main() {
    expect("empty allowlist denies",
           !check_process_allowed({}, "C:\\Windows\\System32\\cmd.exe").allowed);
    expect("empty allowlist explains configuration",
           check_process_allowed({}, "cmd.exe").error.find("process_allowlist") != std::string::npos);
    expect("allowlist of empty strings still denies",
           !check_process_allowed({"", ""}, "cmd.exe").allowed);
    expect("empty executable refused",
           !check_process_allowed({"a.exe"}, "").allowed);

    expect("bare filename entry matches any directory",
           check_process_allowed({"makemkvcon64.exe"},
                                 "C:\\Program Files\\MakeMKV\\makemkvcon64.exe").allowed);
    expect("full path entry matches case-insensitively",
           check_process_allowed({"C:\\Tools\\a.exe"}, "c:\\tools\\A.EXE").allowed);
    expect("basename comparison is case-insensitive",
           check_process_allowed({"FFMPEG.EXE"},
                                 "F:\\AI Tools\\FFmpeg\\bin\\ffmpeg.exe").allowed);
    expect("forward-slash paths split to a basename too",
           check_process_allowed({"x.exe"}, "C:/dir/sub/x.exe").allowed);
    expect("mixed separators split on the last one",
           check_process_allowed({"y.exe"}, "C:\\dir/sub\\y.exe").allowed);

    expect("non-listed executable refused",
           !check_process_allowed({"a.exe"}, "b.exe").allowed);
    expect("refusal names the permitted entries",
           check_process_allowed({"a.exe"}, "b.exe").error.find("a.exe") != std::string::npos);
    expect("full-path entry does not admit a different file of the same directory",
           !check_process_allowed({"C:\\Tools\\a.exe"}, "C:\\Tools\\b.exe").allowed);
    expect("substring of an entry is not a match",
           !check_process_allowed({"powershell.exe"}, "powershell").allowed);

    if (failures) return 1;
    std::cout << "all process-guard tests passed\n";
    return 0;
}
