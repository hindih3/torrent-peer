#include "parser.hpp"
#include <iomanip>
#include <stdexcept>
#include <cctype>

Bencode_value Bencode_parser::parse() {
    Bencode_value value = parse_value();

    if (pos != data.size())
        throw std::runtime_error("Invalid extra data");

    return value;
}

Bencode_value Bencode_parser::parse_value() {
    char c = data[pos];
    if (c == 'i') return parse_int();
    if (c == 'l') return parse_list();
    if (c == 'd') return parse_dict();
    if (isdigit(static_cast<unsigned char>(c))) return parse_string();

    throw std::runtime_error("Invalid bencode value");
}

Bencode_value Bencode_parser::parse_int() {
    ++pos;

    size_t start = pos;

    if (pos < data.size() && data[pos] == '-')
        ++pos;

    while (pos < data.size() && std::isdigit(static_cast<unsigned char>(data[pos])))
        ++pos;

    if (pos >= data.size() || data[pos] != 'e')
        throw std::runtime_error("Unterminated integer");

    std::string num = data.substr(start, pos - start);
    ++pos;

    if (num.empty())
        throw std::runtime_error("Empty integer");
    if (num.size() > 1 && num[0] == '0')
        throw std::runtime_error("Leading zero");
    if (num == "-0")
        throw std::runtime_error("Negative zero");
    if (num == "-")
        throw std::runtime_error("Invalid integer");

    return Bencode_value(std::stoll(num));
}

Bencode_value Bencode_parser::parse_string() {
    size_t colon = pos;
    while (colon < data.size() && std::isdigit(static_cast<unsigned char>(data[colon])))
        ++colon;

    if (colon >= data.size() || data[colon] != ':')
        throw std::runtime_error("Invalid string length");

    size_t length = std::stoull(data.substr(pos, colon - pos));
    pos = colon + 1;

    if (pos + length > data.size())
        throw std::runtime_error("String length out of bounds");

    std::string str(data.data() + pos, length);
    pos += length;

    return Bencode_value(std::move(str));
}

Bencode_value Bencode_parser::parse_list() {
    ++pos;

    Bencode_value::List list;

    while (true) {
        if (pos >= data.size())
            throw std::runtime_error("Unterminated list");

        if (data[pos] == 'e')
            break;

        list.emplace_back(parse_value());
    }
    ++pos;

    return Bencode_value(std::move(list));
}

Bencode_value Bencode_parser::parse_dict() {
    pos++;

    Bencode_value::Dict dict;
    std::string last_key;

    while (true) {
        if (pos >= data.size())
            throw std::runtime_error("Unterminated dict");
        if (data[pos] == 'e')
            break;

        std::string key = parse_string().get_string();

        if (!last_key.empty() && key < last_key)
            throw std::runtime_error("Dictionary keys not sorted");
        last_key = key;

        if (key == "info") info_start = pos;

        Bencode_value value = parse_value();
        dict.emplace(key, value);

        if (key == "info") info_end = pos;
    }

    pos++;
    return Bencode_value(std::move(dict));
}

std::pair<size_t, size_t> Bencode_parser::get_info_range() const {
    return {info_start, info_end};
}

void print_value(const Bencode_value& val, std::ostream& os, int indent) {
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
            for (unsigned char c : s) {
                os << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            }
            os << ">";
            os << std::dec;
        }
    }
    else if (std::holds_alternative<Bencode_value::List>(var)) {
        os << "[\n";
        for (const auto& item : std::get<Bencode_value::List>(var)) {
            os << padding << "  ";
            print_value(item, os, indent + 2);
            os << "\n";
        }
        os << padding << "]";
    }
    else if (std::holds_alternative<Bencode_value::Dict>(var)) {
        os << "{\n";
        for (const auto& [k, v] : std::get<Bencode_value::Dict>(var)) {
            os << padding << "  " << k << ": ";
            print_value(v, os, indent + 2);
            os << "\n";
        }
        os << padding << "}";
    }
}