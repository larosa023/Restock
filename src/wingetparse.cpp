// Restock - reading winget's output (see wingetparse.h)
#include "wingetparse.h"
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <algorithm>
#include "json.h"

namespace wg {

std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)towlower(c);
    return r;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

static bool StartsWithI(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && Lower(s.substr(0, p.size())) == Lower(p);
}

static bool ContainsI(const std::wstring& s, const std::wstring& n) { return Lower(s).find(Lower(n)) != std::wstring::npos; }

int CellWidth(wchar_t c) {
    unsigned u = (unsigned)c;
    if (u >= 0xDC00 && u <= 0xDFFF) return 0;            // low surrogate: the pair counts once
    if (u >= 0xD800 && u <= 0xDBFF) return 2;            // emoji and other astral characters
    if (u >= 0x0300 && u <= 0x036F) return 0;            // combining marks
    if (u == 0x200B || u == 0x200D || u == 0xFE0F) return 0;
    if ((u >= 0x1100 && u <= 0x115F) || (u >= 0x2E80 && u <= 0x303E) || (u >= 0x3041 && u <= 0x33FF) ||
        (u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0xA000 && u <= 0xA4CF) ||
        (u >= 0xAC00 && u <= 0xD7A3) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFE30 && u <= 0xFE4F) ||
        (u >= 0xFF00 && u <= 0xFF60) || (u >= 0xFFE0 && u <= 0xFFE6))
        return 2;
    return 1;
}

// ---------------------------------------------------------------- cleaning
static std::wstring StripEscapes(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == 0x1B && i + 1 < s.size()) {
            wchar_t n = s[i + 1];
            if (n == L'[') {                               // CSI ... final byte
                i += 2;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7E)) ++i;
            } else if (n == L']') {                        // OSC ... BEL or ESC backslash
                i += 2;
                while (i < s.size() && s[i] != 0x07 && !(s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == L'\\')) ++i;
                if (i < s.size() && s[i] == 0x1B) ++i;
            } else {
                ++i;                                       // two-character sequence
            }
            continue;
        }
        if (c == 0x1B || c == 0x07 || c == 0x08 || c == 0xFEFF) continue;
        out += c;
    }
    return out;
}

static bool IsSpinner(const std::wstring& t) {
    std::wstring x = Trim(t);
    return x.size() == 1 && (x == L"-" || x == L"\\" || x == L"|" || x == L"/");
}

std::vector<std::wstring> CleanLines(const std::wstring& raw) {
    std::wstring s = StripEscapes(raw);
    std::vector<std::wstring> lines;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find(L'\n', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring line = s.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        // a progress redraw: what's on screen is whatever was written after the last carriage return
        size_t cr = line.rfind(L'\r');
        if (cr != std::wstring::npos) line = line.substr(cr + 1);
        // trailing spaces don't matter; leading ones do (they're column positions)
        size_t last = line.find_last_not_of(L' ');
        line = last == std::wstring::npos ? L"" : line.substr(0, last + 1);
        if (IsSpinner(line)) line.clear();
        lines.push_back(line);
        if (e == s.size()) break;
    }
    return lines;
}

// ---------------------------------------------------------------- tables
// Cells [from, to) of a line, measured in console cells
static std::wstring CellSlice(const std::wstring& line, int from, int to) {
    std::wstring out;
    int pos = 0;
    bool lastIn = false;
    for (wchar_t ch : line) {
        int w = CellWidth(ch);
        if (w == 0) { if (lastIn) out += ch; continue; }   // a combining mark or the second half of a pair
        bool in = pos >= from && (to < 0 || pos < to);
        if (in) out += ch;
        lastIn = in;
        pos += w;
    }
    return out;
}

static std::vector<int> CellChars(const std::wstring& line) {
    // the character at each cell (index into line), -1 for the second half of a wide character
    std::vector<int> cells;
    for (size_t i = 0; i < line.size(); ++i) {
        int w = CellWidth(line[i]);
        if (w == 0) continue;
        cells.push_back((int)i);
        if (w == 2) cells.push_back(-1);
    }
    return cells;
}

static bool CellIsSpace(const std::wstring& line, const std::vector<int>& cells, int pos) {
    if (pos < 0 || pos >= (int)cells.size()) return true;
    int ci = cells[pos];
    return ci >= 0 && (line[ci] == L' ' || line[ci] == L'\t');
}

static bool IsSeparator(const std::wstring& l) {
    std::wstring t = Trim(l);
    return t.size() >= 8 && t.find_first_not_of(L'-') == std::wstring::npos;
}

static bool LooksLikeVersion(const std::wstring& v) {
    for (wchar_t c : v) if (iswdigit(c)) return true;
    return v == L"Unknown" || (!v.empty() && (v[0] == L'<' || v[0] == L'>'));
}

bool ParseTables(const std::wstring& raw, std::vector<Row>& rows) {
    std::vector<std::wstring> lines = CleanLines(raw);
    bool found = false;
    for (size_t sep = 1; sep < lines.size(); ++sep) {
        if (!IsSeparator(lines[sep])) continue;
        // header: the closest non-empty line above the separator
        size_t h = sep - 1;
        while (h > 0 && Trim(lines[h]).empty()) --h;
        const std::wstring& header = lines[h];
        if (Trim(header).empty()) continue;
        std::vector<int> hcells = CellChars(header);
        // the rows (until a line that can't be one)
        size_t end = sep + 1;
        while (end < lines.size() && !Trim(lines[end]).empty() && !IsSeparator(lines[end])) ++end;
        // candidate column starts: where a header word begins after a space
        std::vector<int> starts;
        for (int p = 0; p < (int)hcells.size(); ++p) {
            if (CellIsSpace(header, hcells, p)) continue;
            if (p == 0 || CellIsSpace(header, hcells, p - 1)) starts.push_back(p);
        }
        if (starts.size() < 3) continue;
        // a header can have words with spaces in other languages; a real column has at least one row
        // whose value begins exactly there
        std::vector<int> cols{starts[0]};
        for (size_t k = 1; k < starts.size(); ++k) {
            int p = starts[k];
            bool confirmed = false;
            for (size_t r = sep + 1; r < end && !confirmed; ++r) {
                std::vector<int> rc = CellChars(lines[r]);
                if (!CellIsSpace(lines[r], rc, p) && CellIsSpace(lines[r], rc, p - 1)) confirmed = true;
            }
            if (confirmed) cols.push_back(p);
        }
        if (cols.size() < 3) continue;
        found = true;
        size_t n = cols.size();
        std::vector<Row> table;
        for (size_t r = sep + 1; r < end; ++r) {
            const std::wstring& line = lines[r];
            std::vector<std::wstring> cell(n);
            for (size_t c = 0; c < n; ++c) cell[c] = Trim(CellSlice(line, cols[c], c + 1 < n ? cols[c + 1] : -1));
            Row row;
            row.name = cell[0];
            row.id = cell[1];
            row.version = cell[2];
            // a sentence ("2 upgrades available."), not a row: values start right at column boundaries
            std::vector<int> rc = CellChars(line);
            bool aligned = CellIsSpace(line, rc, cols[1] - 1) && !CellIsSpace(line, rc, cols[1]) &&
                           CellIsSpace(line, rc, cols[2] - 1) && !CellIsSpace(line, rc, cols[2]);
            if (!aligned || row.id.find(L' ') != std::wstring::npos || row.name.empty() || row.version.empty()) break;
            if (n >= 5) { row.available = cell[3]; row.source = cell[4]; }
            else if (n == 4) (LooksLikeVersion(cell[3]) ? row.available : row.source) = cell[3];
            table.push_back(row);
        }
        // a 4-column table: decide once for the whole column, not per row
        if (n == 4) {
            bool anyVersion = false;
            for (auto& t : table) anyVersion |= !t.available.empty() && LooksLikeVersion(t.available) && t.available != L"winget" && t.available != L"msstore";
            for (auto& t : table) {
                std::wstring v = t.available.empty() ? t.source : t.available;
                t.available.clear(); t.source.clear();
                (anyVersion ? t.available : t.source) = v;
            }
        }
        rows.insert(rows.end(), table.begin(), table.end());
        sep = end - 1;
    }
    return found;
}

// ---------------------------------------------------------------- export
bool ParseExport(const std::wstring& text, std::vector<std::pair<std::wstring, std::wstring>>& out) {
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) return false;
    const json::Value* sources = v.Get(L"Sources");
    if (!sources || sources->type != json::Value::Array) return false;
    for (auto& s : sources->arr) {
        std::wstring src;
        if (const json::Value* d = s.Get(L"SourceDetails")) src = d->GetStr(L"Name");
        const json::Value* pk = s.Get(L"Packages");
        if (!pk || pk->type != json::Value::Array) continue;
        for (auto& p : pk->arr) {
            std::wstring id = p.GetStr(L"PackageIdentifier");
            if (!id.empty()) out.emplace_back(id, src);
        }
    }
    return true;
}

bool IsTruncated(const std::wstring& s) { return !s.empty() && (s.back() == 0x2026 || (s.size() > 3 && s.compare(s.size() - 3, 3, L"...") == 0)); }

std::wstring ResolveTruncated(const std::wstring& shortId, const std::vector<std::wstring>& fullIds) {
    std::wstring prefix = shortId;
    if (!prefix.empty() && prefix.back() == 0x2026) prefix.pop_back();
    else if (prefix.size() > 3 && prefix.compare(prefix.size() - 3, 3, L"...") == 0) prefix.resize(prefix.size() - 3);
    if (prefix.empty()) return L"";
    std::wstring hit;
    for (auto& f : fullIds) {
        if (f.size() >= prefix.size() && StartsWithI(f, prefix)) {
            if (!hit.empty() && Lower(hit) != Lower(f)) return L"";   // ambiguous
            hit = f;
        }
    }
    return hit;
}

// ---------------------------------------------------------------- progress
static double UnitBytes(const std::wstring& u) {
    std::wstring x = Lower(u);
    if (x == L"kb" || x == L"kib") return 1024.0;
    if (x == L"mb" || x == L"mib") return 1024.0 * 1024;
    if (x == L"gb" || x == L"gib") return 1024.0 * 1024 * 1024;
    if (x == L"b") return 1;
    return 0;
}

int ParseProgress(const std::wstring& tail) {
    // "  ██████▒▒▒▒  12.5 MB / 40.1 MB"
    size_t slash = tail.rfind(L" / ");
    if (slash != std::wstring::npos) {
        auto number = [&](size_t from, double& val, std::wstring& unit, bool backward) -> bool {
            (void)backward;
            const wchar_t* p = tail.c_str() + from;
            wchar_t* e = nullptr;
            val = wcstod(p, &e);
            if (e == p) return false;
            while (*e == L' ') ++e;
            const wchar_t* u = e;
            while (*e && iswalpha(*e)) ++e;
            unit.assign(u, e - u);
            return UnitBytes(unit) > 0;
        };
        // the number before " / ": walk back over the unit and the number
        size_t i = slash;
        while (i > 0 && iswalpha(tail[i - 1])) --i;
        while (i > 0 && tail[i - 1] == L' ') --i;
        while (i > 0 && (iswdigit(tail[i - 1]) || tail[i - 1] == L'.' || tail[i - 1] == L',')) --i;
        double a = 0, b = 0;
        std::wstring ua, ub;
        if (number(i, a, ua, true) && number(slash + 3, b, ub, false)) {
            double done = a * UnitBytes(ua), total = b * UnitBytes(ub);
            if (total > 0) return (int)std::min(100.0, done * 100.0 / total);
        }
    }
    size_t pct = tail.rfind(L'%');
    if (pct != std::wstring::npos && pct > 0) {
        size_t i = pct;
        while (i > 0 && iswdigit(tail[i - 1])) --i;
        if (i < pct && pct - i <= 3) {
            int v = (int)wcstol(tail.substr(i, pct - i).c_str(), nullptr, 10);
            if (v >= 0 && v <= 100) return v;
        }
    }
    return -1;
}

std::wstring Phase(const std::wstring& output) {
    std::vector<std::wstring> lines = CleanLines(output);
    for (size_t k = lines.size(); k-- > 0;) {
        std::wstring t = Trim(lines[k]);
        if (t.empty()) continue;
        if (StartsWithI(t, L"Downloading") || ParseProgress(t) >= 0) return L"Downloading…";
        if (ContainsI(t, L"installer hash") || ContainsI(t, L"verified")) return L"Verified";
        if (StartsWithI(t, L"Starting package install") || StartsWithI(t, L"Starting package upgrade")) return L"Installing…";
        if (ContainsI(t, L"Extracting")) return L"Extracting…";
        if (StartsWithI(t, L"Found ")) return L"Starting…";
    }
    return L"";
}

// ---------------------------------------------------------------- results
static std::wstring LastMeaningfulLine(const std::wstring& output) {
    std::vector<std::wstring> lines = CleanLines(output);
    for (size_t k = lines.size(); k-- > 0;) {
        std::wstring t = Trim(lines[k]);
        if (t.empty() || ParseProgress(t) >= 0 || StartsWithI(t, L"http") || StartsWithI(t, L"Downloading") ||
            StartsWithI(t, L"Found ") || StartsWithI(t, L"This application is licensed") ||
            StartsWithI(t, L"Microsoft is not responsible") || IsSeparator(t))
            continue;
        if (t.size() > 160) t = t.substr(0, 157) + L"…";
        return t;
    }
    return L"";
}

static std::wstring InstallerExitCode(const std::wstring& output) {
    // "Installer failed with exit code: 1603"
    std::wstring lo = Lower(output);
    size_t p = lo.find(L"exit code:");
    if (p == std::wstring::npos) return L"";
    p += 10;
    while (p < lo.size() && lo[p] == L' ') ++p;
    size_t e = p;
    while (e < lo.size() && (iswdigit(lo[e]) || lo[e] == L'-' || iswxdigit(lo[e]) || lo[e] == L'x')) ++e;
    return output.substr(p, e - p);
}

Result Classify(uint32_t code, const std::wstring& output) {
    Result r;
    bool restartText = ContainsI(output, L"Restart your PC") || ContainsI(output, L"restart is required") || ContainsI(output, L"reboot");
    std::wstring inst = InstallerExitCode(output);
    auto set = [&](Outcome o, const std::wstring& m) { r.outcome = o; r.message = m; return r; };
    switch (code) {
        case 0:
            if (ContainsI(output, L"No applicable upgrade") || ContainsI(output, L"No available upgrade") || ContainsI(output, L"No newer package"))
                return set(Outcome::UpToDate, L"Already up to date");
            if (restartText) return set(Outcome::Restart, L"Updated · restart your PC to finish");
            return set(Outcome::Updated, L"Updated");
        case 0x8A15002B: return set(Outcome::UpToDate, L"Already up to date");
        case 0x8A15010D: return set(Outcome::UpToDate, L"Already up to date");
        case 0x8A15010E: return set(Outcome::UpToDate, L"A newer version is already installed");
        case 0x8A150109: return set(Outcome::Restart, L"Updated · restart your PC to finish");
        case 0x8A15010B: return set(Outcome::Restart, L"Updated · your PC is restarting to finish");
        case 0x8A15010A: return set(Outcome::Failed, L"Restart your PC first, then update again");
        case 0x8A150101: case 0x8A150111: return set(Outcome::InUse, L"The app is in use. Close it and try again");
        case 0x8A150103: return set(Outcome::InUse, L"One of its files is in use. Close the app and try again");
        case 0x8A150102: return set(Outcome::Failed, L"Another install is running. Try again when it's done");
        case 0x8A150105: return set(Outcome::Failed, L"Not enough disk space");
        case 0x8A150106: return set(Outcome::Failed, L"Not enough memory");
        case 0x8A150107: return set(Outcome::Failed, L"No internet connection");
        case 0x8A150104: return set(Outcome::Failed, L"A component it needs is missing");
        case 0x8A15010C: return set(Outcome::Cancelled, L"Cancelled in the installer");
        case 0x8A15010F: return set(Outcome::Failed, L"Blocked by your organization's policy");
        case 0x8A150113: return set(Outcome::Failed, L"Not supported on this PC");
        case 0x8A150011: return set(Outcome::Failed, L"The download didn't match its checksum, so it wasn't installed");
        case 0x8A150008: return set(Outcome::Failed, L"The download failed");
        case 0x8A150014: return set(Outcome::Failed, L"winget couldn't find this app any more");
        case 0x8A150019: return set(Outcome::NeedsAdmin, L"Needs administrator rights");
        case 0x80070005: return set(Outcome::NeedsAdmin, L"Needs administrator rights");
        case 0x800704C7: return set(Outcome::Cancelled, L"Cancelled");
        default: break;
    }
    if (code == 0x8A150006 || !inst.empty()) {
        if (inst == L"1223") return set(Outcome::Cancelled, L"The administrator prompt was declined");
        if (inst == L"740") return set(Outcome::NeedsAdmin, L"Needs administrator rights");
        if (inst == L"3010" || inst == L"1641") return set(Outcome::Restart, L"Updated · restart your PC to finish");
        if (inst == L"1618") return set(Outcome::Failed, L"Another install is running. Try again when it's done");
        if (inst == L"1602") return set(Outcome::Cancelled, L"Cancelled in the installer");
        if (!inst.empty()) return set(Outcome::Failed, L"The installer failed (exit code " + inst + L")");
    }
    if (ContainsI(output, L"administrator") || ContainsI(output, L"elevat")) return set(Outcome::NeedsAdmin, L"Needs administrator rights");
    if (restartText && (ContainsI(output, L"Successfully installed") || ContainsI(output, L"Successfully upgraded")))
        return set(Outcome::Restart, L"Updated · restart your PC to finish");
    std::wstring last = LastMeaningfulLine(output);
    wchar_t hex[32];
    swprintf(hex, 32, L"winget error 0x%08X", (unsigned)code);
    return set(Outcome::Failed, last.empty() ? std::wstring(hex) : last);
}

} // namespace wg
