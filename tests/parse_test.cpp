// Unit tests for winget output parsing. Build and run on any OS:
//   g++ -std=c++17 -I../src parse_test.cpp ../src/wingetparse.cpp -o parse_test && ./parse_test
#include "wingetparse.h"
#include <cstdio>
#include <string>
#include <locale>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string N(const std::wstring& w) { std::string s; for (wchar_t c : w) s += c < 128 ? (char)c : '?'; return s; }

int main() {
    // winget list: spinner noise, VT colors, a CJK name (two cells per character), a shortened ID
    std::wstring list =
        L"   - \r   \\ \r   | \r\x1b[2K\r"
        L"Name                         Id                              Version        Available     Source\r\n"
        L"--------------------------------------------------------------------------------------------------\r\n"
        L"7-Zip 24.08 (x64)            7zip.7zip                       24.08          24.09         winget\r\n"
        L"\x8C37\x6B4C\x6D4F\x89C8\x5668                   Google.Chrome                   131.0.6778.86                winget\r\n"
        L"Microsoft Visual Studio Co\x2026  Microsoft.VisualStudio.2022.Co\x2026 17.11.5        17.12.0       winget\r\n"
        L"Some Tool                    ARP\\Machine\\X64\\{1234-5678}     1.0\r\n"
        L"Windows Terminal             Microsoft.WindowsTerminal       1.21.3231.0                  msstore\r\n";
    std::vector<wg::Row> rows;
    CHECK(wg::ParseTables(list, rows));
    CHECK(rows.size() == 5);
    if (rows.size() == 5) {
        CHECK(rows[0].name == L"7-Zip 24.08 (x64)");
        CHECK(rows[0].id == L"7zip.7zip");
        CHECK(rows[0].version == L"24.08");
        CHECK(rows[0].available == L"24.09");
        CHECK(rows[0].source == L"winget");
        CHECK(rows[1].id == L"Google.Chrome");
        CHECK(rows[1].version == L"131.0.6778.86");
        CHECK(rows[1].available.empty());
        CHECK(rows[1].source == L"winget");
        CHECK(wg::IsTruncated(rows[2].id));
        CHECK(rows[3].id == L"ARP\\Machine\\X64\\{1234-5678}");
        CHECK(rows[3].source.empty());
        CHECK(rows[4].source == L"msstore");
        printf("row2 id=%s ver=%s avail=%s\n", N(rows[2].id).c_str(), N(rows[2].version).c_str(), N(rows[2].available).c_str());
    }
    std::vector<std::wstring> full{L"Microsoft.VisualStudio.2022.Community", L"7zip.7zip"};
    CHECK(wg::ResolveTruncated(L"Microsoft.VisualStudio.2022.Co\x2026", full) == L"Microsoft.VisualStudio.2022.Community");
    full.push_back(L"Microsoft.VisualStudio.2022.Core");
    CHECK(wg::ResolveTruncated(L"Microsoft.VisualStudio.2022.Co\x2026", full).empty());

    // winget upgrade: two tables (the second needs explicit targeting) and a footer; "< 1.0" versions
    std::wstring up =
        L"Name               Id                    Version      Available     Source\n"
        L"--------------------------------------------------------------------------\n"
        L"Notepad++ (64-bit) Notepad++.Notepad++   8.6.9        8.7.1         winget\n"
        L"Discord            Discord.Discord       < 1.0.9170   1.0.9171      winget\n"
        L"2 upgrades available.\n"
        L"\n"
        L"The following packages have an upgrade available, but require explicit targeting for upgrade:\n"
        L"Name    Id              Version  Available  Source\n"
        L"--------------------------------------------------\n"
        L"Python  Python.Python.3 3.12.1   3.12.7     winget\n";
    rows.clear();
    CHECK(wg::ParseTables(up, rows));
    CHECK(rows.size() == 3);
    if (rows.size() == 3) {
        CHECK(rows[0].id == L"Notepad++.Notepad++");
        CHECK(rows[0].available == L"8.7.1");
        CHECK(rows[1].version == L"< 1.0.9170");
        CHECK(rows[2].id == L"Python.Python.3");
        CHECK(rows[2].available == L"3.12.7");
    }

    // a 4-column list (no updates at all, so no Available column)
    std::wstring four =
        L"Name      Id              Version  Source\n"
        L"-----------------------------------------\n"
        L"VLC       VideoLAN.VLC    3.0.21   winget\n"
        L"Thing     ARP\\User\\X64\\t  2.0\n";
    rows.clear();
    CHECK(wg::ParseTables(four, rows));
    CHECK(rows.size() == 2);
    if (rows.size() == 2) { CHECK(rows[0].source == L"winget"); CHECK(rows[0].available.empty()); }

    // a header word with a space in another language ("Versión disponible") isn't split into two columns
    std::wstring es =
        L"Nombre  Id            Versión  Versión disponible  Origen\n"
        L"---------------------------------------------------------\n"
        L"VLC     VideoLAN.VLC  3.0.20   3.0.21              winget\n";
    rows.clear();
    CHECK(wg::ParseTables(es, rows));
    CHECK(rows.size() == 1);
    if (rows.size() == 1) { CHECK(rows[0].available == L"3.0.21"); CHECK(rows[0].source == L"winget"); }

    // no table
    rows.clear();
    CHECK(!wg::ParseTables(L"No installed package found matching input criteria.\n", rows));

    // export
    std::vector<std::pair<std::wstring, std::wstring>> ex;
    CHECK(wg::ParseExport(L"{\"Sources\":[{\"Packages\":[{\"PackageIdentifier\":\"7zip.7zip\",\"Version\":\"24.08\"}],\"SourceDetails\":{\"Name\":\"winget\"}}]}", ex));
    CHECK(ex.size() == 1 && ex[0].first == L"7zip.7zip" && ex[0].second == L"winget");

    // progress
    CHECK(wg::ParseProgress(L"  \x2588\x2588\x2588\x2588\x2592\x2592\x2592\x2592  10.0 MB / 40.0 MB") == 25);
    CHECK(wg::ParseProgress(L"  \x2588\x2588\x2588\x2588\x2592\x2592  1024 KB / 2.00 MB") == 50);
    CHECK(wg::ParseProgress(L"  \x2588\x2588\x2588  45%") == 45);
    CHECK(wg::ParseProgress(L"Starting package install...") == -1);

    // outcomes
    CHECK(wg::Classify(0, L"Successfully installed").outcome == wg::Outcome::Updated);
    CHECK(wg::Classify(0x8A15002B, L"No applicable upgrade found.").outcome == wg::Outcome::UpToDate);
    CHECK(wg::Classify(0x8A150109, L"").outcome == wg::Outcome::Restart);
    CHECK(wg::Classify(0x8A150006, L"Installer failed with exit code: 1603").message == L"The installer failed (exit code 1603)");
    CHECK(wg::Classify(0x8A150006, L"Installer failed with exit code: 1223").outcome == wg::Outcome::Cancelled);
    CHECK(wg::Classify(0x8A150006, L"Installer failed with exit code: 3010").outcome == wg::Outcome::Restart);
    CHECK(wg::Classify(0x8A150101, L"").outcome == wg::Outcome::InUse);
    CHECK(wg::Classify(0x80070005, L"").outcome == wg::Outcome::NeedsAdmin);
    CHECK(wg::Classify(0x8A15FFFF, L"Found X [x]\nSomething specific went wrong\n").message == L"Something specific went wrong");
    CHECK(wg::Phase(L"Found X\nDownloading https://x\n  \x2588\x2588 1 MB / 2 MB") == L"Downloading…");
    CHECK(wg::Phase(L"Successfully verified installer hash\nStarting package install...\n") == L"Installing…");

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("all parse tests passed\n");
    return 0;
}
