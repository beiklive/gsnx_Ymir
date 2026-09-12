#include <ymir/util/string.hpp>

#include <array>

// For string <-> wstring conversions
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <stringapiset.h>
#elif defined(__SWITCH__)
    // newlib on Switch ships iconv.h but no iconv implementation; do the
    // conversion manually instead.
    #include <cassert>
#else
    #include <cassert>
    #include <iconv.h>
#endif

namespace util {

struct ReplacementChar {
    const char *normal = nullptr;
    const char *dakuten = nullptr;
    const char *handakuten = nullptr;
};

std::string TranslateSaturnString(std::string_view str) {
    static constexpr std::array<ReplacementChar, 256> kTable = {{
#include "jp_char_table.inc"
    }};

    std::string output;
    output.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        const auto ch = static_cast<unsigned char>(str[i]);
        const ReplacementChar &entry = kTable[ch];

        // Look ahead for dakuten or handakuten
        if (i + 1 < str.size()) {
            const auto next = static_cast<unsigned char>(str[i + 1]);

            if (next == 0xDE) {
                // Dakuten suffix
                if (entry.dakuten) {
                    output += entry.dakuten;
                    ++i;
                    continue;
                } else {
                    output += entry.normal;
                    // output += "゛";
                    output += "¨"; // font doesn't have the standalone symbol
                    ++i;
                    continue;
                }
            } else if (next == 0xDF) {
                // Handakuten suffix
                if (entry.handakuten) {
                    output += entry.handakuten;
                    ++i;
                    continue;
                } else {
                    output += entry.normal;
                    // output += "゜";
                    output += "°"; // font doesn't have the standalone symbol
                    ++i;
                    continue;
                }
            }
        }

        // No suffix
        output += entry.normal;
    }

    return output;
}

std::string TrimWhitespace(std::string str) {
    auto start = str.find_first_not_of(" ");
    auto end = str.find_last_not_of(" ");

    if (start == std::string::npos && end == std::string::npos) {
        // The entire string is whitespace
        return "";
    }
    if (start == std::string::npos) {
        start = 0;
    }
    if (end == std::string::npos) {
        end = str.size();
    }
    return str.substr(start, end + 1);
}

std::wstring StringToWString(std::string_view str) {
    if (str.empty()) {
        return L"";
    }

#ifdef _WIN32
    // Windows implementation
    const int size = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size);
    return wstr;

#elif defined(__SWITCH__)
    // Manual UTF-8 -> UTF-32 (wchar_t is 32-bit on aarch64) conversion.
    std::wstring wstr;
    wstr.reserve(str.size());
    size_t i = 0;
    while (i < str.size()) {
        const unsigned char c = static_cast<unsigned char>(str[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return L"";
        }
        if (i + extra >= str.size()) {
            return L"";
        }
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(str[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return L"";
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        wstr.push_back(static_cast<wchar_t>(cp));
        i += extra + 1;
    }
    return wstr;

#else
    // Linux/macOS/FreeBSD implementation

    // Open converted
    iconv_t cd = iconv_open("WCHAR_T", "UTF-8");
    assert(cd != (iconv_t)-1);

    // Gather parameters
    char *inBuf = const_cast<char *>(str.data());
    size_t inRemaining = str.size();

    size_t outLength = str.size();
    size_t outRemaining = outLength * sizeof(wchar_t);

    std::wstring wstr(outLength, L'\0');
    char *outBuf = reinterpret_cast<char *>(wstr.data());

    // Convert string
    size_t result = iconv(cd, &inBuf, &inRemaining, &outBuf, &outRemaining);
    if (result == (size_t)-1) {
        iconv_close(cd);
        // TODO: handle error
        // throw std::runtime_error("iconv conversion failed due to an invalid character sequence.");
        return L"";
    }
    iconv_close(cd);

    size_t bytesWritten = outLength * sizeof(wchar_t) - outRemaining;
    size_t charsWritten = bytesWritten / sizeof(wchar_t);
    wstr.resize(charsWritten);

    return wstr;

#endif
}

std::string WStringToString(std::wstring_view wstr) {
    if (wstr.empty()) {
        return "";
    }

#ifdef _WIN32
    // Windows implementation
    const int size = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str[0], size, nullptr, nullptr);
    return str;

#elif defined(__SWITCH__)
    // Manual UTF-32 (wchar_t is 32-bit on aarch64) -> UTF-8 conversion.
    std::string str;
    str.reserve(wstr.size() * 3);
    for (wchar_t wc : wstr) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp <= 0x7F) {
            str.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            str.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            str.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            str.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            str.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            str.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            str.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return str;

#else
    // Linux/macOS/FreeBSD implementation

    // Open converted
    iconv_t cd = iconv_open("UTF-8", "WCHAR_T");
    assert(cd != (iconv_t)-1);

    // Gather parameters
    char *inBuf = reinterpret_cast<char *>(const_cast<wchar_t *>(wstr.data()));
    size_t inRemaining = wstr.size() * sizeof(wchar_t);

    // Worst case for UTF-8 is 4 bytes per character
    size_t outLength = wstr.size() * 4;
    size_t outRemaining = outLength;

    std::string str(outLength, '\0');
    char *outBuf = str.data();

    // Convert string
    size_t result = iconv(cd, &inBuf, &inRemaining, &outBuf, &outRemaining);
    if (result == (size_t)-1) {
        iconv_close(cd);
        // TODO: handle error
        // throw std::runtime_error("iconv conversion failed due to an invalid character sequence.");
        return "";
    }
    iconv_close(cd);

    size_t bytesWritten = outLength - outRemaining;
    size_t charsWritten = bytesWritten;
    str.resize(charsWritten);

    return str;

#endif
}

} // namespace util
