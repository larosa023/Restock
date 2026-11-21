// Restock - reading winget's output. Plain C++ (no Windows headers) so it's unit-tested on any OS
// (tests/parse_test.cpp).
//
// winget has no machine-readable output for `list` and `upgrade`, so Restock reads its tables. They
// are laid out in console cells: column starts come from the header line, East Asian characters take
// two cells, and older winget versions shorten long values with "…" when the output is redirected.
// Shortened IDs are completed from `winget export`, which always writes full IDs.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace wg {

struct Row {
    std::wstring name, id, version, available, source;
};

// Removes VT escape sequences and progress redraws (text overwritten with '\r'), and splits lines.
std::vector<std::wstring> CleanLines(const std::wstring& raw);

// Every table in winget's output (`upgrade` can print two: the second lists packages that need to be
// named explicitly). Returns false if there's no table at all.
bool ParseTables(const std::wstring& raw, std::vector<Row>& rows);

// winget export JSON -> (id, source name) pairs
bool ParseExport(const std::wstring& json, std::vector<std::pair<std::wstring, std::wstring>>& out);

// Completes an ID that winget shortened with "…". Returns "" if it's not unique.
std::wstring ResolveTruncated(const std::wstring& shortId, const std::vector<std::wstring>& fullIds);
bool IsTruncated(const std::wstring& s);

// Progress in the latest output while an update runs: "12.5 MB / 40.1 MB" or "45%". -1 if none.
int ParseProgress(const std::wstring& tail);

// What happened, from winget's exit code and output
enum class Outcome { Updated, UpToDate, Restart, NeedsAdmin, InUse, Cancelled, Failed };
struct Result {
    Outcome outcome = Outcome::Failed;
    std::wstring message;          // short and plain, shown on the app's row and in History
};
Result Classify(uint32_t exitCode, const std::wstring& output);

// The phase an update is in, from the last lines of output ("Downloading…", "Installing…"), or "".
std::wstring Phase(const std::wstring& output);

int CellWidth(wchar_t c);
std::wstring Lower(const std::wstring& s);
std::wstring Trim(const std::wstring& s);

} // namespace wg
