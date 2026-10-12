/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_virtual_instance_metadata.h"
#include "lib/time/ob_time_utility.h"
#include "share/instance_meta/instance_meta_value_codec.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using share::instance_meta::InstanceTable;
using share::instance_meta::FieldType;
using share::instance_meta::instance_table_definition;
using share::instance_meta::InstanceMetaValueCodec;
namespace
{
// Keep existing diagnostic labels; these no longer identify KV collections.
constexpr uint64_t diagnostic_ids[] = {1, 2, 5, 6, 7};
void json_string(const ObString &text, std::string &out)
{
  static const char hex[] = "0123456789abcdef";
  out += '"';
  for (int64_t i = 0; i < text.length(); ++i) {
    const unsigned char c = text.ptr()[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else { out += c; }
  }
  out += '"';
}
void json_cell(const ObObj &cell, std::string &out, FieldType type, bool key = false)
{
  if (cell.is_null()) { out += "null"; }
  else if (cell.is_integer_type()) {
    if (key) { out += '"'; }
    out += cell.is_uint64() ? std::to_string(cell.get_uint64()) : std::to_string(cell.get_int());
    if (key) { out += '"'; }
  } else if (type == FieldType::BYTES) {
    std::string base64;
    InstanceMetaValueCodec::base64(cell.get_string(), base64);
    out += "{\"encoding\":\"base64\",\"data\":";
    json_string(ObString(base64.size(), base64.data()), out);
    out += '}';
  } else { json_string(cell.get_string(), out); }
}
} // namespace

InstanceMetadataTable::InstanceMetadataTable(storage::InstanceMetaStore &store)
    : store_(store), tx_(), started_(false), has_lower_(false), table_index_(0),
      last_key_(), key_json_(), key_hex_(), value_json_(), value_base64_(),
      value_size_(0), value_is_json_(false)
{}

InstanceMetadataTable::~InstanceMetadataTable() { reset(); }

void InstanceMetadataTable::reset()
{
  if (tx_.is_active()) { (void)store_.rollback(tx_); }
  started_ = false;
  has_lower_ = false;
  table_index_ = 0;
  last_key_.clear();
  key_json_.clear();
  key_hex_.clear();
  value_json_.clear();
  value_base64_.clear();
  value_size_ = 0;
  value_is_json_ = false;
  ObVirtualTableScannerIterator::reset();
}

bool InstanceMetadataTable::has_column(uint64_t id) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < output_column_ids_.count(); ++i) {
    found = output_column_ids_.at(i) == id;
  }
  return found;
}

void InstanceMetadataTable::set_text(ObObj &cell, const std::string &value, bool lob) const
{
  if (lob) { cell.set_lob_value(ObLongTextType, value.data(), value.size()); }
  else { cell.set_varchar(value.data(), value.size()); }
  cell.set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
}

int InstanceMetadataTable::load_next()
{
  int ret = OB_SUCCESS;
  if (!started_) {
    ret = store_.begin(tx_, ObTimeUtility::current_time() + 120 * 1000 * 1000, true);
    if (ret == OB_SUCCESS) { started_ = true; }
  }
  const int64_t table_count = static_cast<int64_t>(InstanceTable::STORAGE_LAYOUT_KV);
  while (ret == OB_SUCCESS && table_index_ < table_count) {
    const auto table = static_cast<InstanceTable>(table_index_);
    const auto &definition = *instance_table_definition(table);
    // Keep a string bound stable while the callback advances the cursor.
    const std::string lower_bytes = last_key_;
    ObObj lower = last_key_cell_;
    if (has_lower_ && definition.fields[0].type == FieldType::TEXT) {
      lower.set_varbinary(ObString(lower_bytes.size(), lower_bytes.data()));
    }
    bool found = false;
    ret = store_.scan(tx_, table, [&](const ObNewRow &row, bool &stop) {
      if (row.count_ != definition.field_count) { return OB_CHECKSUM_ERROR; }
      key_json_ = "{";
      value_json_ = "{";
      key_hex_.clear();
      value_base64_.clear();
      for (int64_t i = 0; i < row.count_; ++i) {
        const bool key = i < definition.key_columns;
        std::string &out = key ? key_json_ : value_json_;
        if (out.size() > 1) { out += ','; }
        json_string(ObString::make_string(definition.fields[i].name), out);
        out += ':';
        json_cell(row.cells_[i], out, definition.fields[i].type, key);
      }
      key_json_ += '}';
      value_json_ += '}';
      last_key_cell_ = row.cells_[0];
      last_key_.clear();
      if (last_key_cell_.is_string_type()) {
        const auto &bytes = last_key_cell_.get_string();
        last_key_.assign(bytes.ptr(), bytes.length());
      }
      has_lower_ = true;
      // A single opaque payload is displayed as bytes for any such table.
      value_is_json_ = !(definition.field_count == definition.key_columns + 1
          && definition.fields[definition.key_columns].type == FieldType::BYTES);
      value_size_ = value_json_.size();
      if (!value_is_json_) {
        const auto &bytes = row.cells_[definition.key_columns].get_string();
        value_size_ = bytes.length();
        value_json_.clear();
        if (has_column(VALUE_BASE64)) { InstanceMetaValueCodec::base64(bytes, value_base64_); }
      }
      if (has_column(KEY_HEX)) {
        std::string bytes = last_key_;
        if (row.cells_[0].is_uint64()) {
          const uint64_t id = row.cells_[0].get_uint64();
          for (int i = 7; i >= 0; --i) { bytes += static_cast<char>(id >> (8 * i)); }
        }
        static const char hex[] = "0123456789abcdef";
        for (unsigned char c : bytes) { key_hex_ += hex[c >> 4]; key_hex_ += hex[c & 15]; }
      }
      found = stop = true;
      return OB_SUCCESS;
    }, has_lower_ ? &lower : nullptr);
    if (ret == OB_SUCCESS && found) { break; }
    if (ret == OB_SUCCESS) { ++table_index_; has_lower_ = false; last_key_.clear(); }
  }
  if (ret == OB_SUCCESS && table_index_ == table_count) {
    if (tx_.is_active()) { ret = store_.rollback(tx_); }
    if (ret == OB_SUCCESS) { ret = OB_ITER_END; }
  }
  return ret;
}

int InstanceMetadataTable::fill_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  const auto *desc = instance_table_definition(static_cast<InstanceTable>(table_index_));
  if (desc == nullptr || cur_row_.cells_ == nullptr) { return OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; ret == OB_SUCCESS && i < output_column_ids_.count(); ++i) {
    ObObj &cell = cur_row_.cells_[i];
    switch (output_column_ids_.at(i)) {
      case COLLECTION_ID: cell.set_uint64(diagnostic_ids[table_index_]); break;
      case COLLECTION_NAME:
        cell.set_varchar(desc->name);
        cell.set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
        break;
      case KEY_JSON: set_text(cell, key_json_, true); break;
      case KEY_HEX: set_text(cell, key_hex_, true); break;
      case VALUE_FORMAT:
        cell.set_varchar(value_is_json_ ? "JSON" : "BYTES");
        cell.set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
        break;
      case VALUE_JSON:
        if (value_is_json_) { set_text(cell, value_json_, true); }
        else { cell.set_null(); }
        break;
      case VALUE_SIZE: cell.set_int(value_size_); break;
      case VALUE_BASE64:
        if (value_is_json_) { cell.set_null(); }
        else { set_text(cell, value_base64_, true); }
        break;
      default: ret = OB_ERR_UNEXPECTED; break;
    }
  }
  if (ret == OB_SUCCESS) { row = &cur_row_; }
  return ret;
}

int InstanceMetadataTable::inner_get_next_row(ObNewRow *&row)
{
  int ret = load_next();
  if (ret == OB_SUCCESS) { ret = fill_row(row); }
  return ret;
}

} // namespace observer
} // namespace oceanbase
