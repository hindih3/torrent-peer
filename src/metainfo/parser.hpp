#pragma once
#include <variant>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstdint>

class Bencode_value {
public:
    using List    = std::vector<Bencode_value>;
    using Dict    = std::map<std::string, Bencode_value>;
    using Variant = std::variant<int64_t, std::string, List, Dict>;

    Bencode_value(int64_t v) : value_(v) {}
    Bencode_value(const std::string& v) : value_(v) {}
    Bencode_value(const List& v) : value_(v) {}
    Bencode_value(const Dict& v) : value_(v) {}

    Bencode_value(std::string&& v) : value_(std::move(v)) {}
    Bencode_value(List&& v) : value_(std::move(v)) {}
    Bencode_value(Dict&& v) : value_(std::move(v)) {}

    bool is_int() const { return std::holds_alternative<int64_t>(value_); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_list() const { return std::holds_alternative<List>(value_); }
    bool is_dict() const { return std::holds_alternative<Dict>(value_); }

    int64_t get_int() const { return std::get<int64_t>(value_); }
    const std::string& get_string() const { return std::get<std::string>(value_); }
    const List& get_list() const { return std::get<List>(value_); }
    const Dict& get_dict() const { return std::get<Dict>(value_); }
    List& get_list() { return std::get<List>(value_); }
    Dict& get_dict() { return std::get<Dict>(value_); }

    const Variant& get_variant() const { return value_; }

private:
    Variant value_;
};

class Bencode_parser {
public:
    explicit Bencode_parser(const std::string& input) : data_(input) {}

    Bencode_value parse();
    Bencode_value parse_value();
    Bencode_value parse_int();
    Bencode_value parse_string();
    Bencode_value parse_list();
    Bencode_value parse_dict();

    [[nodiscard]] std::pair<size_t, size_t> get_info_range() const;

private:
    const std::string& data_;
    size_t pos_ = 0;

    size_t info_start = 0;
    size_t info_end   = 0;

    static constexpr size_t max_depth = 100;
    size_t depth_ = 0;

    std::string read_string();
};

void print_value(const Bencode_value& val, std::ostream& os = std::cout, int indent = 0);