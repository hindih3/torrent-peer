#include <variant>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <algorithm>
#include <cctype>

using llong = long long;

class Bencode_value {
public:
    using List = std::vector<Bencode_value>;
    using Dict = std::map<std::string, Bencode_value>;
    using Variant = std::variant<llong, std::string, List, Dict>;

private:
    Variant value;

public:
    Bencode_value(llong v) : value(v) {}
    Bencode_value(const std::string& v) : value(v) {}
    Bencode_value(const List& v) : value(v) {}
    Bencode_value(const Dict& v) : value(v) {}

    bool is_int() const { return std::holds_alternative<llong>(value); }
    bool is_string() const { return std::holds_alternative<std::string>(value); }
    bool is_list() const { return std::holds_alternative<List>(value); }
    bool is_dict() const { return std::holds_alternative<Dict>(value); }

    llong get_int() const { return std::get<llong>(value); }
    const std::string& get_string() const { return std::get<std::string>(value); }
    const List& get_list() const { return std::get<List>(value); }
    const Dict& get_dict() const { return std::get<Dict>(value); }
    List& get_list() { return std::get<List>(value); }
    Dict& get_dict() { return std::get<Dict>(value); }

    const Variant& get_variant() const { return value; }

    friend std::ostream& operator<<(std::ostream& os, const Bencode_value& val);
};

class Bencode_parser {
private:
    const std::string& data;
    size_t pos = 0;
public:
    Bencode_parser(const std::string& input) : data(input) {}

    Bencode_value parse() {
        Bencode_value value = parse_value();

        while (pos < data.size() && std::isspace(data[pos]))
            ++pos;
        if (pos != data.size())
            throw std::runtime_error("Invalid extra data");

        return value;
    }

    Bencode_value parse_value() {
        if (pos >= data.size())
            throw std::runtime_error("Unexpected end of input");

        char c = data[pos];

        if (c == 'i') return parse_int();
        if (c == 'l') return parse_list();
        if (c == 'd') return parse_dict();
        if (isdigit(c)) return parse_string();

        throw std::runtime_error("Invalid bencode value");
    }

    Bencode_value parse_int() {
        ++pos;

        size_t end = data.find('e', pos);
        if (end == std::string::npos)
            throw std::runtime_error("Unterminated integer");

        std::string num = data.substr(pos, end - pos);

        if (num.empty())
            throw std::runtime_error("Empty integer");

        if (num.size() > 1 && num[0] == '0')
            throw std::runtime_error("Leading zero");

        if (num == "-0")
            throw std::runtime_error("Negative zero");

        size_t start = (num[0] == '-') ? 1 : 0;
        for (size_t i = start; i < num.size(); ++i) {
            if (!isdigit(num[i]))
                throw std::runtime_error("Invalid integer");
        }

        long long value = std::stoll(num);

        pos = end + 1;
        return Bencode_value(value);
    }

    Bencode_value parse_string() {
        size_t colon = data.find(':', pos);

        if (colon == std::string::npos)
            throw std::runtime_error("Invalid string length");

        int length = std::stoi(data.substr(pos, colon - pos));
        pos = colon + 1;

        if (pos + length > data.size())
            throw std::runtime_error("String length out of bounds");

        std::string_view str_view(&data[pos], length);
        pos += length;

        return Bencode_value(std::string(str_view));
    }

    Bencode_value parse_list() {
        ++pos;

        Bencode_value::List list;

        while (true) {
            if (pos >= data.size())
                throw std::runtime_error("Unterminated list");

            if (data[pos] == 'e')
                break;

            list.push_back(parse_value());
        }
        ++pos;

        return Bencode_value(list);
    }

    Bencode_value parse_dict() {
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

            Bencode_value value = parse_value();
            dict.emplace(key, value);
        }

        pos++;
        return Bencode_value(dict);
    }
};

std::ostream& operator<<(std::ostream& os, const Bencode_value& val) {
    const auto& var = val.get_variant();

    if (std::holds_alternative<llong>(var)) {
        os << std::get<llong>(var);
    } 
    else if (std::holds_alternative<std::string>(var)) {
        const std::string& s = std::get<std::string>(var);

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
                os << std::hex << (int)c;
            }
            os << ">";
            os << std::dec;
        }
    }
    else if (std::holds_alternative<Bencode_value::List>(var)) {
        os << "[ ";
        for (const auto& item : std::get<Bencode_value::List>(var))
            os << item << " ";
        os << "]";
    } 
    else if (std::holds_alternative<Bencode_value::Dict>(var)) {
        os << "{ ";
        for (const auto& [k, v] : std::get<Bencode_value::Dict>(var))
            os << k << ": " << v << " ";
        os << "}";
    }

    return os;
}

void print_value(const Bencode_value& val, std::ostream& os, int indent = 0) {
    const auto& var = val.get_variant();
    std::string padding(indent, ' ');

    if (std::holds_alternative<llong>(var)) {
        os << std::get<llong>(var);
    } 
    else if (std::holds_alternative<std::string>(var)) {
        const std::string& s = std::get<std::string>(var);

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
                os << std::hex << (int)c;
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