#pragma once

#include <algorithm>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sizetree {

// Every whitespace-separated term must match as a case-insensitive subsequence.
// Bonuses favor adjacent letters and word boundaries; term order is irrelevant.
// Scratch buffers are reused across candidates; no filesystem access is needed.
class FuzzyMatcher {
public:
    explicit FuzzyMatcher(std::string_view query = {}) { reset(query); }

    void reset(std::string_view query) {
        std::wstring decoded;
        decode(query, decoded);
        terms_.clear();
        std::size_t longest = 0;
        for (std::size_t begin = 0; begin < decoded.size();) {
            if (space(decoded[begin])) { ++begin; continue; }
            auto end = begin + 1;
            while (end < decoded.size() && !space(decoded[end])) ++end;
            auto& term = terms_.emplace_back(decoded, begin, end - begin);
            for (auto& letter : term) letter = fold(letter);
            longest = std::max(longest, term.size());
            begin = end;
        }
        best_.resize(longest);
        previous_.resize(longest);
    }

    bool empty() const { return terms_.empty(); }

    std::optional<int> score(std::string_view text) {
        if (empty()) return std::nullopt;
        decode(text, text_);
        foldedText_.resize(text_.size());
        std::transform(text_.begin(), text_.end(), foldedText_.begin(), fold);
        // Reject missing terms before computing the more expensive ranking.
        for (const auto& term : terms_) {
            std::size_t matched = 0;
            for (const auto letter : foldedText_) {
                if (letter == term[matched] && ++matched == term.size()) break;
            }
            if (matched != term.size()) return std::nullopt;
        }
        boundaries_.resize(text_.size());
        for (std::size_t i = 0; i < text_.size(); ++i) {
            boundaries_[i] = i == 0 || text_[i - 1] > 0x10ffff || !std::iswalnum(text_[i - 1]) ||
                (text_[i] <= 0x10ffff && std::iswlower(text_[i - 1]) && std::iswupper(text_[i]));
        }
        int result = 0;
        for (const auto& term : terms_) result += scoreTerm(term);
        return result;
    }

private:
    std::vector<std::wstring> terms_;
    std::wstring text_, foldedText_;
    std::vector<unsigned char> boundaries_;
    std::vector<int> best_, previous_;

    int scoreTerm(std::wstring_view term) {
        constexpr int absent = INT_MIN / 4;
        std::fill_n(best_.begin(), term.size(), absent);
        std::fill_n(previous_.begin(), term.size(), absent);
        int result = absent;
        for (std::size_t i = 0; i < text_.size(); ++i) {
            for (std::size_t j = term.size(); j-- > 0;) {
                int current = absent;
                if (foldedText_[i] == term[j]) {
                    if (j == 0) current = -static_cast<int>(std::min<std::size_t>(i, 128));
                    else current = std::max(best_[j - 1] - 3, previous_[j - 1] + 20);
                    current += 32 + (boundaries_[i] ? 12 : 0);
                }
                previous_[j] = current;
                best_[j] = std::max(best_[j] - 1, current);
                if (j + 1 == term.size()) result = std::max(result, current);
            }
        }
        result -= static_cast<int>(std::min<std::size_t>(text_.size(), 128));
        if (text_.size() == term.size()) result += 200;
        return result;
    }

    static bool space(wchar_t letter) {
        return letter <= 0x10ffff && std::iswspace(letter);
    }

    static wchar_t fold(wchar_t letter) {
        if (letter >= L'A' && letter <= L'Z') return letter + (L'a' - L'A');
        if (letter < 128 || letter > 0x10ffff) return letter;
        return static_cast<wchar_t>(std::towlower(letter));
    }

    static void decode(std::string_view input, std::wstring& output) {
        output.clear();
        for (std::size_t offset = 0; offset < input.size();) {
            const auto byte = static_cast<unsigned char>(input[offset]);
            if (byte < 128) {
                output.push_back(byte);
                ++offset;
                continue;
            }
            std::mbstate_t state{};
            wchar_t letter{};
            const auto length = std::mbrtowc(&letter, input.data() + offset, input.size() - offset, &state);
            if (length == static_cast<std::size_t>(-1) || length == static_cast<std::size_t>(-2) || length == 0) {
                output.push_back(static_cast<wchar_t>(0x110000 + byte));
                ++offset;
            } else {
                output.push_back(letter);
                offset += length;
            }
        }
    }
};

} // namespace sizetree
