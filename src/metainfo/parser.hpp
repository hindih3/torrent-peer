#pragma once
#include <variant>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <utility>

class Bencode_value {
public:
    using List    = std::vector<Bencode_value>;
    using Dict    = std::map<std::string, Bencode_value>;
    using Variant = std::variant<int64_t, std::string, List, Dict>;

private:
    Variant value;

public:
    Bencode_value(int64_t v) : value(v) {}
    Bencode_value(const std::string& v) : value(v) {}
    Bencode_value(const List& v) : value(v) {}
    Bencode_value(const Dict& v) : value(v) {}

    Bencode_value(std::string&& v) : value(std::move(v)) {}
    Bencode_value(List&& v) : value(std::move(v)) {}
    Bencode_value(Dict&& v) : value(std::move(v)) {}

    bool is_int() const { return std::holds_alternative<int64_t>(value); }
    bool is_string() const { return std::holds_alternative<std::string>(value); }
    bool is_list() const { return std::holds_alternative<List>(value); }
    bool is_dict() const { return std::holds_alternative<Dict>(value); }

    int64_t get_int() const { return std::get<int64_t>(value); }
    const std::string& get_string() const { return std::get<std::string>(value); }
    const List& get_list() const { return std::get<List>(value); }
    const Dict& get_dict() const { return std::get<Dict>(value); }
    List& get_list() { return std::get<List>(value); }
    Dict& get_dict() { return std::get<Dict>(value); }

    const Variant& get_variant() const { return value; }
};

class Bencode_parser {
private:
    const std::string& data;
    size_t pos = 0;

    size_t info_start = 0;
    size_t info_end   = 0;

public:
    explicit Bencode_parser(const std::string& input) : data(input) {}

    Bencode_value parse();
    Bencode_value parse_value();
    Bencode_value parse_int();
    Bencode_value parse_string();
    Bencode_value parse_list();
    Bencode_value parse_dict();

    [[nodiscard]] std::pair<size_t, size_t> get_info_range() const;
};

void print_value(const Bencode_value& val, std::ostream& os = std::cout, int indent = 0);