#include <algorithm>
#include <cassert>
#include <cctype>
#include <fstream>
#include <functional>
#include <ranges>
#include <spdlog/spdlog.h>

#include "Record/Record.hpp"
#include "SqlParser.hpp"

SqlParser::~SqlParser() {
  delete m_sc;
  delete m_parser;
}

void SqlParser::parse(const char *filename) {
  assert(filename != nullptr);
  std::ifstream in_file(filename);
  if (!in_file.good()) {
    exit(EXIT_FAILURE);
  }
  parse_helper(in_file);
}

auto SqlParser::parse(std::istream &stream) -> ParserResponse & {
  if (!stream.good() && stream.eof()) {
    return this->m_parser_response;
  }
  parse_helper(stream);
  return this->m_parser_response;
}

void SqlParser::parse_helper(std::istream &stream) {
  delete (m_sc);
  try {
    m_sc = new scanner(&stream);
  } catch (std::bad_alloc &ba) {
    spdlog::error("Failed to allocate scanner: ({})", ba.what());
    throw ba;
  }
  delete (m_parser);
  try {
    m_parser = new yy::parser((*m_sc), (*this));
  } catch (std::bad_alloc &ba) {
    spdlog::error("Failed to allocate parser: ({})", ba.what());
    throw ba;
  }

  const int ACCEPT(0);
  if (m_parser->parse() != ACCEPT) {
    spdlog::error("Parsing failed");
    throw std::runtime_error("Parsing failed");
  }
}

void SqlParser::check_table_name(const std::string &tablename) {
  spdlog::info("Cheking Table : {}", tablename);
  if (!this->m_engine.is_table(tablename)) {
    spdlog::error("Table doesn't exists");
    throw std::runtime_error("Table doesn't exists");
  }
}

void SqlParser::create_table(const std::string &tablename,
                             const std::vector<column_t> &columns) {

  std::vector<Type> col_types;
  std::vector<std::string> col_names;

  std::string primary_key;

  col_types.reserve(columns.size());
  col_names.reserve(columns.size());

  for (const auto &col : columns) {
    if (col.is_pk) {
      primary_key = col.name;
    }

    col_types.push_back(col.type);
    col_names.push_back(col.name);
  }

  m_engine.create_table(tablename, primary_key, col_types, col_names);
}

void SqlParser::create_index(const std::string &tablename,
                             const std::string &column_name,
                             const DB_ENGINE::DBEngine::Index_t &index_name) {

  // Validate table
  if (!m_engine.is_table(tablename)) {
    spdlog::error("Table doesn't exists");
    throw std::runtime_error("Table doesn't exists");
  }

  // Validate attribute
  auto attributes = m_engine.get_table_attributes(tablename);
  if (std::ranges::find(attributes, column_name) == attributes.end()) {
    spdlog::error("Column doesn't exists");
    throw std::runtime_error("Column doesn't exists");
  }

  m_engine.create_index(tablename, column_name, index_name);
}

void SqlParser::select(const std::string &tablename,
                       const std::vector<std::string> &column_names,
                       const std::list<std::list<condition_t>> &constraints) {
  auto sorted_column_names = m_engine.sort_attributes(tablename, column_names);

  const auto &table_attributes = this->m_engine.get_table_attributes(tablename);

  QueryResponse query_response;

  // check if col exists
  if (std::ranges::any_of(sorted_column_names, [&](const auto &col) {
        return std::ranges::find(table_attributes, col) ==
               table_attributes.end();
      })) {
    spdlog::error("Column doesn't exists");
    throw std::runtime_error("Column doesn't exists");
  }

  // No indexed attribute found
  if (constraints.empty()) {
    query_response = m_engine.load(tablename, sorted_column_names);
    spdlog::info("Query response size: {}", query_response.records.size());
    query_to_output(query_response, sorted_column_names);
    return;
  }

  // Iterating OR constraints
  for (const auto &or_constraint : constraints) {

    condition_t constraint_key;
    std::vector<std::function<bool(const DB_ENGINE::Record &rec)>> lambdas;

    // Iterating the AND contraints
    for (const auto &column_constraint : or_constraint) {
      // Checkear si el constraint actual tiene un indice asociado
      // si lo tiene, asignar al constraint_key
      // si no, construir un predicado con los operadores;

      spdlog::info("Column constraint: {}", column_constraint.column_name);

      auto indexes = this->m_engine.get_indexes_names(tablename);

      // If the column doesnt has an index
      if (std::ranges::find(indexes, column_constraint.column_name) ==
          indexes.end()) {

        auto record_comp = m_engine.get_comparator(
            tablename, column_constraint.c, column_constraint.column_name,
            column_constraint.value);

        lambdas.push_back(record_comp);

        // If the column has an index and the constraint_key is empty
      } else if (constraint_key.column_name.empty()) {
        constraint_key = column_constraint;
      }
    }

    // Convert vec of lambdas to a single one
    spdlog::info("Lambdas size: {}", lambdas.size());
    auto joined_lambdas = [lambdas](const Record &rec) {
      return std::ranges::all_of(lambdas, [&](const auto &single_lambda) {
        return single_lambda(rec);
      });
    };

    // No indexed key in constraints, performing linear search
    if (constraint_key.column_name.empty()) {
      spdlog::error("INIT LOAD");
      query_response =
          m_engine.load(tablename, sorted_column_names, joined_lambdas);
      spdlog::error("INIT LOADED {}", query_response.records.size());
      break;
    }

    QueryResponse or_response;
    if (constraint_key.c == Comp::EQUAL) {
      or_response = {m_engine.search(
          tablename, {constraint_key.column_name, constraint_key.value},
          joined_lambdas, sorted_column_names)};

    } else {
      Attribute begin_key = DB_ENGINE::KEY_LIMITS::MIN;
      Attribute end_key = DB_ENGINE::KEY_LIMITS::MAX;

      switch (constraint_key.c) {
      case Comp::L:
      case Comp::LE:
        end_key = {constraint_key.column_name, constraint_key.value};
        break;
      case Comp::G:
      case Comp::GE:
        begin_key = {constraint_key.column_name, constraint_key.value};
        break;
      case Comp::EQUAL:
        break;
      }
      or_response = m_engine.range_search(tablename, begin_key, end_key,
                                          joined_lambdas, sorted_column_names);
    }

    query_response.query_times =
        merge_times(query_response.query_times, or_response.query_times);
    query_response.records =
        merge_records(query_response.records, or_response.records);
  }
  query_to_output(query_response, sorted_column_names);
}

void SqlParser::query_to_output(
    const DB_ENGINE::QueryResponse &query_response,
    const std::vector<std::string> &sorted_column_names) {
  m_parser_response.records = query_response.records;
  m_parser_response.query_times = query_response.query_times;
  m_parser_response.table_names = m_engine.get_table_names();
  m_parser_response.column_names = sorted_column_names;
}

auto SqlParser::merge_records(const std::vector<Record> &vec1,
                              const std::vector<Record> &vec2)
    -> std::vector<Record> {

  std::vector<Record> response;
  response.reserve(vec1.size() + vec2.size());

  response.insert(response.end(), vec1.begin(), vec1.end());

  std::unordered_set<Record, RecordHash> unique_elements(vec1.begin(),
                                                         vec1.end());

  for (const auto &element : vec2) {
    if (unique_elements.find(element) == unique_elements.end()) {
      response.push_back(element);
      unique_elements.insert(element);
    }
  }

  return response;
}

auto SqlParser::merge_times(query_time_t &times_1, const query_time_t &times_2)
    -> query_time_t & {
  times_1.insert(times_2.begin(), times_2.end());
  return times_1;
}

void SqlParser::insert_from_file(const std::string &tablename,
                                 const std::string &filename) {
  auto file_name = filename.substr(1, filename.length() - 2);
  m_engine.csv_insert(tablename, file_name);
}

void SqlParser::insert(const std::string &tablename,
                       const std::vector<std::string> &values) {

  m_engine.add(tablename, {values.rbegin(), values.rend()});
}

void SqlParser::remove(const std::string &tablename,
                       std::list<std::list<condition_t>> &constraint) {
  Attribute key;
  condition_t &unique_condition = constraint.front().front();
  key.name = unique_condition.column_name;
  key.value = unique_condition.value;
  m_engine.remove(tablename, key);
}

void SqlParser::drop_table(const std::string &tablename) {
  m_engine.drop_table(tablename);
}

void SqlParser::select_between(const std::string &tablename,
                               const std::vector<std::string> &column_names,
                               const std::string &id, const std::string &val1,
                               const std::string &val2) {
  auto sorted_column_names = column_names;
  m_engine.sort_attributes(tablename, sorted_column_names);

  auto table_attributes = this->m_engine.get_table_attributes(tablename);

  QueryResponse query_response;

  // check if col exists
  if (std::ranges::any_of(sorted_column_names, [&](const auto &col) {
        return std::ranges::find(table_attributes, col) ==
               table_attributes.end();
      })) {
    spdlog::error("Column doesn't exists");
    throw std::runtime_error("Column doesn't exists");
  }

  Attribute begin_key = {id, val1};
  Attribute end_key = {id, val2};

  query_response = m_engine.range_search(tablename, begin_key, end_key, {},
                                         sorted_column_names);

  query_to_output(query_response, sorted_column_names);
}
