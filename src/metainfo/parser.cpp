#include "parser.hpp"
#include <iomanip>
#include <stdexcept>
#include <cctype>
#include <charconv>
#include <system_error>

BencodeValue Bencode_parser::parse() {
    BencodeValue value = parse_value();

    if (pos_ != data_.size())
        throw std::runtime_error("Invalid extra data");

    return value;
}

BencodeValue Bencode_parser::parse_value() {
    if (pos_ >= data_.size())
        throw std::runtime_error("Unexpected end of input");

    char c = data_[pos_];
    if (c == 'i') return parse_int();
    if (c == 'l') return parse_list();
    if (c == 'd') return parse_dict();
    if (isdigit(static_cast<unsigned char>(c))) return parse_string();

    throw std::runtime_error("Invalid bencode value");
}

BencodeValue Bencode_parser::parse_int() {
    ++pos_;    // skip 'i'
    const size_t start = pos_;

    if (pos_ < data_.size() && data_[pos_] == '-')
        ++pos_;
    const bool negative = pos_ != start;

    const size_t digits_start = pos_;
    while (pos_ < data_.size() && std::isdigit(static_cast<unsigned char>(data_[pos_])))
        ++pos_;
    const size_t digit_count = pos_ - digits_start;

    if (pos_ >= data_.size())
        throw std::runtime_error("Unterminated integer");
    if (data_[pos_] != 'e')
        throw std::runtime_error("Unexpected character in integer");
    if (digit_count == 0)
        throw std::runtime_error("Integer has no digits");

    if (data_[digits_start] == '0' && (digit_count > 1 || negative))
        throw std::runtime_error("Leading zero or negative zero");

    const char* first = data_.data() + start;
    const char* last  = data_.data() + pos_;
    int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(first, last, value);

    if (ec == std::errc::result_out_of_range)
        throw std::runtime_error("Integer out of range");
    if (ec != std::errc{} || ptr != last)
        throw std::runtime_error("Invalid integer");
    ++pos_;    // skip 'e'
    return BencodeValue(value);
}

BencodeValue Bencode_parser::parse_string() {
    return {read_string()};
}

BencodeValue Bencode_parser::parse_list() {
    if (++depth_ > kMaxDepth)
        throw std::runtime_error("Nesting too deep");
    ++pos_;    // skip 'l'

    BencodeValue::List list;
    while (true) {
        if (pos_ >= data_.size())
            throw std::runtime_error("Unterminated list");
        if (data_[pos_] == 'e')
            break;
        list.emplace_back(parse_value());
    }

    ++pos_;    // skip 'e'
    --depth_;
    return {std::move(list)};
}

BencodeValue Bencode_parser::parse_dict() {
    if (++depth_ > kMaxDepth)
        throw std::runtime_error("Nesting too deep");
    ++pos_;                                   // skip 'd'

    BencodeValue::Dict dict;
    while (true) {
        if (pos_ >= data_.size())
            throw std::runtime_error("Unterminated dict");
        if (data_[pos_] == 'e')
            break;

        std::string key = read_string();

        // keys arrive in sorted order, so the map's last key is the previous key
        if (!dict.empty() && key <= dict.rbegin()->first)
            throw std::runtime_error(key == dict.rbegin()->first
                                         ? "Duplicate dictionary key"
                                         : "Dictionary keys not sorted");

        const bool is_info = depth_ == 1 && key == "info";
        if (is_info) info_start = pos_;

        BencodeValue value = parse_value();

        if (is_info) info_end = pos_;

        dict.emplace_hint(dict.end(), std::move(key), std::move(value));
    }

    ++pos_;                                   // skip 'e'
    --depth_;
    return {std::move(dict)};
}

std::pair<size_t, size_t> Bencode_parser::get_info_range() const {
    return {info_start, info_end};
}

std::string Bencode_parser::read_string() {
    const size_t digits_start = pos_;
    while (pos_ < data_.size() && std::isdigit(static_cast<unsigned char>(data_[pos_])))
        ++pos_;
    const size_t digit_count = pos_ - digits_start;

    if (pos_ >= data_.size())
        throw std::runtime_error("Unterminated string length");
    if (data_[pos_] != ':')
        throw std::runtime_error("Unexpected character in string length");
    if (digit_count == 0)
        throw std::runtime_error("String length has no digits");
    if (data_[digits_start] == '0' && digit_count > 1)
        throw std::runtime_error("Leading zero in string length");

    const char* first = data_.data() + digits_start;
    const char* last  = data_.data() + pos_;
    size_t length = 0;
    const auto [ptr, ec] = std::from_chars(first, last, length);

    if (ec == std::errc::result_out_of_range)
        throw std::runtime_error("String length out of range");
    if (ec != std::errc{} || ptr != last)
        throw std::runtime_error("Invalid string length");

    ++pos_;    // skip ':'

    // written this way so it can't overflow: pos <= data.size() here
    if (length > data_.size() - pos_)
        throw std::runtime_error("String length out of bounds");

    std::string str = data_.substr(pos_, length);
    pos_ += length;
    return str;
}

void print_value(const BencodeValue& val, std::ostream& os, int indent) {
    const auto& var = val.get_variant();
    std::string padding(indent, ' ');

    if (std::holds_alternative<int64_t>(var)) {
        os << std::get<int64_t>(var);
    }
    else if (std::holds_alternative<std::string>(var)) {
        const auto& s = std::get<std::string>(var);

        bool printable = true;
        for (unsigned char c : s) {
            if (!std::isprint(c)) {
                printable = false;
                break;
            }
        }

        if (printable) {
            os << '"' << s << '"';
        } else {
            os << "<hex:";
            for (const unsigned char c : s) {
                os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
            }
            os << ">";
            os << std::dec;
        }
    }
    else if (std::holds_alternative<BencodeValue::List>(var)) {
        os << "[\n";
        for (const auto& item : std::get<BencodeValue::List>(var)) {
            os << padding << "  ";
            print_value(item, os, indent + 2);
            os << "\n";
        }
        os << padding << "]";
    }
    else if (std::holds_alternative<BencodeValue::Dict>(var)) {
        os << "{\n";
        for (const auto& [k, v] : std::get<BencodeValue::Dict>(var)) {
            os << padding << "  " << k << ": ";
            print_value(v, os, indent + 2);
            os << "\n";
        }
        os << padding << "}";
    }
}