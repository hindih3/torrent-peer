#pragma once
#include <variant>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstdint>

class BencodeValue {
public:
    using List    = std::vector<BencodeValue>;
    using Dict    = std::map<std::string, BencodeValue>;
    using Variant = std::variant<int64_t, std::string, List, Dict>;

    explicit BencodeValue(int64_t v) : value_(v) {}
    BencodeValue(const std::string& v) : value_(v) {}
    BencodeValue(const List& v) : value_(v) {}
    BencodeValue(const Dict& v) : value_(v) {}

    BencodeValue(std::string&& v) : value_(std::move(v)) {}
    BencodeValue(List&& v) : value_(std::move(v)) {}
    BencodeValue(Dict&& v) : value_(std::move(v)) {}

    [[nodiscard]] bool is_int() const { return std::holds_alternative<int64_t>(value_); }
    [[nodiscard]] bool is_string() const { return std::holds_alternative<std::string>(value_); }
    [[nodiscard]] bool is_list() const { return std::holds_alternative<List>(value_); }
    [[nodiscard]] bool is_dict() const { return std::holds_alternative<Dict>(value_); }

    [[nodiscard]] int64_t get_int() const { return std::get<int64_t>(value_); }
    [[nodiscard]] const std::string& get_string() const { return std::get<std::string>(value_); }
    [[nodiscard]] const List& get_list() const { return std::get<List>(value_); }
    [[nodiscard]] const Dict& get_dict() const { return std::get<Dict>(value_); }
    List& get_list() { return std::get<List>(value_); }
    Dict& get_dict() { return std::get<Dict>(value_); }

    [[nodiscard]] const Variant& get_variant() const { return value_; }

private:
    Variant value_;
};

class BencodeParser {
public:
    explicit BencodeParser(const std::string& input) : data_(input) {}
    BencodeParser(std::string&&) = delete;

    BencodeValue parse();
    [[nodiscard]] std::pair<size_t, size_t> get_info_range() const;

private:
    const std::string& data_;
    size_t pos_ = 0;

    size_t info_start_ = 0;
    size_t info_end_   = 0;

    static constexpr size_t kMaxDepth = 100;
    size_t depth_ = 0;

    BencodeValue parse_value();
    BencodeValue parse_int();
    BencodeValue parse_string();
    BencodeValue parse_list();
    BencodeValue parse_dict();

    std::string read_string();
};

void print_value(const BencodeValue& val, std::ostream& os = std::cout, int indent = 0);