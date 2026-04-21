#pragma once
#include <vector>
#include <string>

struct UrlSpan {
    int startCol;
    int endCol; // inclusive
};

template<typename F>
inline std::vector<UrlSpan> DetectUrls(int cols, F charAtCol) {
    std::vector<UrlSpan> urls;

    struct Scheme { const wchar_t* prefix; int len; };
    static const Scheme schemes[] = {
        { L"https://", 8 },
        { L"http://", 7 },
        { L"ftp://", 6 },
        { L"file://", 7 },
    };

    for (int c = 0; c < cols; ) {
        bool found = false;
        for (auto& s : schemes) {
            if (c + s.len > cols) continue;
            bool match = true;
            for (int i = 0; i < s.len; i++) {
                if (charAtCol(c + i) != s.prefix[i]) { match = false; break; }
            }
            if (!match) continue;

            int end = c + s.len;
            int parenDepth = 0;
            while (end < cols) {
                wchar_t ch = charAtCol(end);
                if (ch <= L' ' || ch == 0) break;
                if (ch == L'(') parenDepth++;
                else if (ch == L')') {
                    if (parenDepth <= 0) break;
                    parenDepth--;
                }
                end++;
            }
            end--;

            while (end > c + s.len - 1) {
                wchar_t ch = charAtCol(end);
                if (ch != L'.' && ch != L',' && ch != L';' && ch != L':' &&
                    ch != L'\'' && ch != L'"' && ch != L']' && ch != L'>')
                    break;
                end--;
            }

            if (end >= c + s.len) {
                urls.push_back({c, end});
                c = end + 1;
                found = true;
                break;
            }
        }
        if (!found) c++;
    }

    return urls;
}

template<typename F>
inline std::wstring ExtractUrlString(int startCol, int endCol, F charAtCol) {
    std::wstring url;
    for (int c = startCol; c <= endCol; c++) {
        wchar_t ch = charAtCol(c);
        if (ch != 0 && ch > L' ')
            url += ch;
    }
    return url;
}
