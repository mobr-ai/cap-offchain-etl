#include "cap/GovernanceSync.hpp"

#include "cap/Http.hpp"
#include "cap/Json.hpp"
#include "cap/Utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace cap {
namespace {

struct GovApiDocument {
  std::string source_system;
  std::string source_endpoint;
  std::string source_external_id;
  std::string source_url;
  std::string raw_content;
  std::string raw_text;
  std::string content_sha256;
  long http_status{};
  std::string content_type;

  std::string title;
  std::string abstract;
  std::string motivation;
  std::string rationale;

  std::string proposer_name;
  std::string proposer_url;
  std::string proposer_id;

  std::string lifecycle_status;
  std::string governance_action_type;

  std::string governance_action_tx_id;
  std::string governance_action_index;
  std::string governance_action_id;

  std::string metadata_url;
  std::string metadata_hash;

  std::string requested_lovelace;
};

std::string lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string normalize_space(const std::string& input)
{
  std::string output;
  bool previous_space = false;

  for(char c : input) {
    bool is_space = std::isspace(static_cast<unsigned char>(c));

    if(is_space) {
      if(!previous_space) {
        output.push_back(' ');
      }

      previous_space = true;
    } else {
      output.push_back(c);
      previous_space = false;
    }
  }

  return trim(output);
}

std::string json_scalar_to_string(const JsonValue& value)
{
  if(value.type == JsonValue::Type::String) {
    return normalize_space(value.str);
  }

  if(value.type == JsonValue::Type::Number) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(0) << value.number;
    return stream.str();
  }

  if(value.type == JsonValue::Type::Bool) {
    return value.boolean ? "true" : "false";
  }

  return "";
}

std::string compact_json_text(const JsonValue& value);

std::string escape_json_string(const std::string& value)
{
  std::string out;

  for(char c : value) {
    switch(c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }

  return out;
}

std::string compact_json_text(const JsonValue& value)
{
  if(value.type == JsonValue::Type::String) {
    return "\"" + escape_json_string(value.str) + "\"";
  }

  if(value.type == JsonValue::Type::Number) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(0) << value.number;
    return stream.str();
  }

  if(value.type == JsonValue::Type::Bool) {
    return value.boolean ? "true" : "false";
  }

  if(value.type == JsonValue::Type::Array) {
    std::string out = "[";

    for(size_t i = 0; i < value.array.size(); ++i) {
      if(i > 0) {
        out += ",";
      }

      out += compact_json_text(value.array[i]);
    }

    out += "]";
    return out;
  }

  if(value.type == JsonValue::Type::Object) {
    std::string out = "{";
    bool first = true;

    for(const auto& item : value.object) {
      if(!first) {
        out += ",";
      }

      first = false;
      out += "\"" + escape_json_string(item.first) + "\":" + compact_json_text(item.second);
    }

    out += "}";
    return out;
  }

  return "null";
}

void collect_text(const JsonValue& value, std::vector<std::string>& output)
{
  std::string scalar = json_scalar_to_string(value);

  if(!scalar.empty()) {
    output.push_back(scalar);
  }

  if(value.type == JsonValue::Type::Array) {
    for(const auto& item : value.array) {
      collect_text(item, output);
    }
  }

  if(value.type == JsonValue::Type::Object) {
    for(const auto& item : value.object) {
      collect_text(item.second, output);
    }
  }
}

std::string raw_text_from_json(const JsonValue& value)
{
  std::vector<std::string> values;
  collect_text(value, values);

  std::string output;

  for(const auto& item : values) {
    if(!output.empty()) {
      output += " ";
    }

    output += item;
  }

  return normalize_space(output);
}

const JsonValue* object_get_ci(const JsonValue& object, const std::vector<std::string>& keys)
{
  if(object.type != JsonValue::Type::Object) {
    return nullptr;
  }

  for(const auto& wanted : keys) {
    std::string wanted_lower = lower(wanted);

    for(const auto& item : object.object) {
      if(lower(item.first) == wanted_lower) {
        return &item.second;
      }
    }
  }

  return nullptr;
}

std::string find_first_string_recursive(
  const JsonValue& value,
  const std::vector<std::string>& keys
)
{
  if(value.type == JsonValue::Type::Object) {
    const JsonValue* direct = object_get_ci(value, keys);

    if(direct) {
      std::string scalar = json_scalar_to_string(*direct);

      if(!scalar.empty()) {
        return scalar;
      }

      if(direct->type == JsonValue::Type::Object) {
        const JsonValue* nested_value = object_get_ci(*direct, {"value", "@value", "name", "title"});

        if(nested_value) {
          scalar = json_scalar_to_string(*nested_value);

          if(!scalar.empty()) {
            return scalar;
          }
        }
      }
    }

    for(const auto& item : value.object) {
      std::string found = find_first_string_recursive(item.second, keys);

      if(!found.empty()) {
        return found;
      }
    }
  }

  if(value.type == JsonValue::Type::Array) {
    for(const auto& item : value.array) {
      std::string found = find_first_string_recursive(item, keys);

      if(!found.empty()) {
        return found;
      }
    }
  }

  return "";
}

std::vector<const JsonValue*> find_candidate_records(const JsonValue& root)
{
  std::vector<const JsonValue*> output;

  if(root.type == JsonValue::Type::Array) {
    for(const auto& item : root.array) {
      if(item.type == JsonValue::Type::Object) {
        output.push_back(&item);
      }
    }

    return output;
  }

  if(root.type != JsonValue::Type::Object) {
    return output;
  }

  const JsonValue* data = object_get_ci(root, {"data", "items", "results", "governanceActions", "proposals"});

  if(data && data->type == JsonValue::Type::Array) {
    for(const auto& item : data->array) {
      if(item.type == JsonValue::Type::Object) {
        const JsonValue* attrs = object_get_ci(item, {"attributes"});

        if(attrs && attrs->type == JsonValue::Type::Object) {
          output.push_back(attrs);
        } else {
          output.push_back(&item);
        }
      }
    }
  }

  if(output.empty()) {
    output.push_back(&root);
  }

  return output;
}

std::string numeric_only_lovelace(const std::string& value)
{
  std::string out;

  for(char c : value) {
    if(std::isdigit(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }

  return out;
}

std::string detect_action_type(const JsonValue& record, const std::string& raw_text)
{
  std::string value = find_first_string_recursive(
    record,
    {"governanceActionType", "governance_action_type", "actionType", "type"}
  );

  std::string haystack = lower(value + " " + raw_text);

  if(
    haystack.find("treasury") != std::string::npos &&
    haystack.find("withdraw") != std::string::npos
  ) {
    return "treasuryWithdrawals";
  }

  if(haystack.find("info action") != std::string::npos || haystack.find("infoaction") != std::string::npos) {
    return "infoAction";
  }

  if(haystack.find("parameter") != std::string::npos) {
    return "parameterChange";
  }

  if(haystack.find("hard fork") != std::string::npos) {
    return "hardForkInitiation";
  }

  if(haystack.find("constitution") != std::string::npos) {
    return "newConstitution";
  }

  if(haystack.find("no confidence") != std::string::npos) {
    return "motionOfNoConfidence";
  }

  return value;
}

std::string detect_status(const JsonValue& record, const std::string& raw_text)
{
  std::string value = find_first_string_recursive(
    record,
    {"status", "state", "lifecycleStatus", "proposalStatus", "votingStatus"}
  );

  if(!value.empty()) {
    return value;
  }

  std::string haystack = lower(raw_text);

  if(haystack.find("live voting") != std::string::npos || haystack.find("active") != std::string::npos) {
    return "live";
  }

  if(haystack.find("ratified") != std::string::npos) {
    return "ratified";
  }

  if(haystack.find("enacted") != std::string::npos) {
    return "enacted";
  }

  if(haystack.find("expired") != std::string::npos) {
    return "expired";
  }

  if(haystack.find("rejected") != std::string::npos) {
    return "rejected";
  }

  return "";
}

void extract_action_identity(GovApiDocument& doc, const JsonValue& record)
{
  doc.governance_action_tx_id = lower(find_first_string_recursive(
    record,
    {"txId", "txHash", "transactionId", "governanceActionTxId", "transactionHash"}
  ));

  doc.governance_action_index = find_first_string_recursive(
    record,
    {"index", "proposalIndex", "governanceActionIndex", "actionIndex"}
  );

  doc.governance_action_id = find_first_string_recursive(
    record,
    {"id", "governanceActionId", "actionId", "proposalId"}
  );

  std::smatch match;

  if(doc.governance_action_tx_id.empty()) {
    std::regex tx_regex("([0-9a-fA-F]{64})");

    if(std::regex_search(doc.raw_content, match, tx_regex)) {
      doc.governance_action_tx_id = lower(match[1].str());
    }
  }

  if(doc.governance_action_id.empty()) {
    if(!doc.governance_action_tx_id.empty() && !doc.governance_action_index.empty()) {
      doc.governance_action_id =
        doc.governance_action_tx_id + "_" + doc.governance_action_index;
    } else {
      doc.governance_action_id = doc.governance_action_tx_id;
    }
  }
}

std::string detect_requested_lovelace(const JsonValue& record, const std::string& raw_text)
{
  std::string value = find_first_string_recursive(
    record,
    {"requestedLovelace", "amountLovelace", "lovelace", "withdrawalAmount", "amount"}
  );

  value = numeric_only_lovelace(value);

  if(!value.empty()) {
    return value;
  }

  std::smatch match;
  std::regex ada_regex("([0-9][0-9,\\. ]+)\\s*(ADA|ada)");

  if(std::regex_search(raw_text, match, ada_regex)) {
    std::string ada = match[1].str();

    ada.erase(std::remove_if(ada.begin(), ada.end(), [](char c) {
      return c == ',' || c == ' ';
    }), ada.end());

    size_t dot = ada.find('.');
    std::string whole = dot == std::string::npos ? ada : ada.substr(0, dot);
    std::string fraction = dot == std::string::npos ? "" : ada.substr(dot + 1);

    while(fraction.size() < 6) {
      fraction.push_back('0');
    }

    if(fraction.size() > 6) {
      fraction = fraction.substr(0, 6);
    }

    if(std::regex_match(whole, std::regex("[0-9]+"))) {
      return whole + fraction;
    }
  }

  return "";
}

std::string endpoint_url(
  const std::string& source_system,
  const std::string& base,
  const std::string& endpoint,
  int page,
  int limit
)
{
  if(source_system == "outcomes") {
    return base + endpoint +
      "?page=" + std::to_string(page) +
      "&limit=" + std::to_string(limit);
  }

  return base + endpoint +
    "?pagination%5Bpage%5D=" + std::to_string(page) +
    "&pagination%5BpageSize%5D=" + std::to_string(limit) +
    "&populate=deep";
}

std::string nullable_text(const std::string& value)
{
  if(value.empty()) {
    return "NULL";
  }

  return "'" + shell_escape(value) + "'";
}

std::string nullable_int(const std::string& value)
{
  if(value.empty() || !std::regex_match(value, std::regex("[0-9]+"))) {
    return "NULL";
  }

  return value;
}

std::string nullable_numeric(const std::string& value)
{
  if(value.empty() || !std::regex_match(value, std::regex("[0-9]+"))) {
    return "NULL";
  }

  return value;
}

void log_fetch_error(
  Db& db,
  const Config& config,
  const std::string& source_system,
  const std::string& url,
  long http_status,
  const std::string& error
)
{
  db.exec(
    "INSERT INTO offchain_governance_fetch_log("
    "provider,source_system,url,http_status,error"
    ") VALUES("
    "'" + shell_escape(config.gov_provider) + "',"
    "'" + shell_escape(source_system) + "',"
    "'" + shell_escape(url) + "',"
    + std::to_string(http_status) + ","
    "'" + shell_escape(error) + "'"
    ")"
  );
}

bool should_skip_recent(
  Db& db,
  const Config& config,
  const std::string& source_system,
  const std::string& source_endpoint,
  const std::string& source_external_id
)
{
  std::string count = db.scalar(
    "SELECT count(*) FROM offchain_governance_proposal "
    "WHERE provider='" + shell_escape(config.gov_provider) + "' "
    "AND source_system='" + shell_escape(source_system) + "' "
    "AND source_endpoint='" + shell_escape(source_endpoint) + "' "
    "AND source_external_id='" + shell_escape(source_external_id) + "' "
    "AND fetched_at > now() - interval '" +
    std::to_string(config.gov_refetch_hours) + " hours'",
    "0"
  );

  return count != "0";
}

void upsert_document(Db& db, const Config& config, const GovApiDocument& doc)
{
  if(should_skip_recent(
    db,
    config,
    doc.source_system,
    doc.source_endpoint,
    doc.source_external_id
  )) {
    return;
  }

  db.exec(
    "INSERT INTO offchain_governance_proposal("
    "provider,source_system,source_endpoint,source_external_id,source_url,"
    "title,abstract,motivation,rationale,"
    "proposer_name,proposer_url,proposer_id,"
    "lifecycle_status,governance_action_type,"
    "governance_action_tx_id,governance_action_index,governance_action_id,"
    "metadata_url,metadata_hash,requested_lovelace,"
    "raw_content,raw_text,content_sha256,http_status,content_type"
    ") VALUES("
    "'" + shell_escape(config.gov_provider) + "',"
    "'" + shell_escape(doc.source_system) + "',"
    "'" + shell_escape(doc.source_endpoint) + "',"
    "'" + shell_escape(doc.source_external_id) + "',"
    "'" + shell_escape(doc.source_url) + "',"
    + nullable_text(doc.title) + ","
    + nullable_text(doc.abstract) + ","
    + nullable_text(doc.motivation) + ","
    + nullable_text(doc.rationale) + ","
    + nullable_text(doc.proposer_name) + ","
    + nullable_text(doc.proposer_url) + ","
    + nullable_text(doc.proposer_id) + ","
    + nullable_text(doc.lifecycle_status) + ","
    + nullable_text(doc.governance_action_type) + ","
    + nullable_text(doc.governance_action_tx_id) + ","
    + nullable_int(doc.governance_action_index) + ","
    + nullable_text(doc.governance_action_id) + ","
    + nullable_text(doc.metadata_url) + ","
    + nullable_text(doc.metadata_hash) + ","
    + nullable_numeric(doc.requested_lovelace) + ","
    "'" + shell_escape(doc.raw_content) + "',"
    "'" + shell_escape(doc.raw_text) + "',"
    "'" + shell_escape(doc.content_sha256) + "',"
    + std::to_string(doc.http_status) + ","
    + nullable_text(doc.content_type) +
    ") "
    "ON CONFLICT(provider,source_system,source_endpoint,source_external_id,content_sha256) "
    "DO UPDATE SET "
    "last_seen_at=now(),"
    "fetched_at=now(),"
    "source_url=EXCLUDED.source_url,"
    "title=EXCLUDED.title,"
    "abstract=EXCLUDED.abstract,"
    "motivation=EXCLUDED.motivation,"
    "rationale=EXCLUDED.rationale,"
    "proposer_name=EXCLUDED.proposer_name,"
    "proposer_url=EXCLUDED.proposer_url,"
    "proposer_id=EXCLUDED.proposer_id,"
    "lifecycle_status=EXCLUDED.lifecycle_status,"
    "governance_action_type=EXCLUDED.governance_action_type,"
    "governance_action_tx_id=EXCLUDED.governance_action_tx_id,"
    "governance_action_index=EXCLUDED.governance_action_index,"
    "governance_action_id=EXCLUDED.governance_action_id,"
    "metadata_url=EXCLUDED.metadata_url,"
    "metadata_hash=EXCLUDED.metadata_hash,"
    "requested_lovelace=EXCLUDED.requested_lovelace,"
    "raw_content=EXCLUDED.raw_content,"
    "raw_text=EXCLUDED.raw_text,"
    "http_status=EXCLUDED.http_status,"
    "content_type=EXCLUDED.content_type,"
    "enabled=true"
  );
}

GovApiDocument parse_record(
  const std::string& source_system,
  const std::string& source_endpoint,
  const std::string& source_url,
  const HttpResult& response,
  const JsonValue& record
)
{
  GovApiDocument doc;

  doc.source_system = source_system;
  doc.source_endpoint = source_endpoint;
  doc.source_url = source_url;
  doc.raw_content = compact_json_text(record);
  doc.raw_text = raw_text_from_json(record);
  doc.content_sha256 = pseudo_sha256(doc.raw_content);
  doc.http_status = response.status;
  doc.content_type = response.content_type;

  doc.title = find_first_string_recursive(record, {"title", "name", "givenName", "given_name"});
  doc.abstract = find_first_string_recursive(record, {"abstract", "summary", "description"});
  doc.motivation = find_first_string_recursive(record, {"motivation", "motivations"});
  doc.rationale = find_first_string_recursive(record, {"rationale"});

  doc.proposer_name = find_first_string_recursive(
    record,
    {"proposerName", "proposer_name", "proposer", "organization", "groupName", "author", "creator", "submittedBy"}
  );

  doc.proposer_url = find_first_string_recursive(
    record,
    {"proposerUrl", "proposer_url", "website", "webSite", "url"}
  );

  doc.proposer_id = find_first_string_recursive(
    record,
    {"proposerId", "proposer_id", "stakeAddress", "stake_address", "drepId", "drep_id"}
  );

  doc.lifecycle_status = detect_status(record, doc.raw_text);
  doc.governance_action_type = detect_action_type(record, doc.raw_text);

  extract_action_identity(doc, record);

  doc.metadata_url = find_first_string_recursive(
    record,
    {"metadataUrl", "metadata_url", "proposalUrl", "proposal_url"}
  );

  doc.metadata_hash = find_first_string_recursive(
    record,
    {"metadataHash", "metadata_hash", "hash"}
  );

  doc.requested_lovelace = detect_requested_lovelace(record, doc.raw_text);

  std::string explicit_id = find_first_string_recursive(
    record,
    {"documentId", "externalId", "external_id", "id", "slug"}
  );

  if(!doc.governance_action_id.empty()) {
    doc.source_external_id = doc.governance_action_id;
  } else if(!explicit_id.empty()) {
    doc.source_external_id = explicit_id;
  } else {
    doc.source_external_id = hex_id(source_system + ":" + source_endpoint + ":" + doc.raw_content);
  }

  return doc;
}

bool response_has_more(const JsonValue& root, size_t record_count, int limit)
{
  if(record_count >= static_cast<size_t>(limit)) {
    return true;
  }

  std::string page_count = find_first_string_recursive(root, {"pageCount", "totalPages"});
  std::string page = find_first_string_recursive(root, {"page"});

  if(!page.empty() && !page_count.empty()) {
    try {
      return std::stoi(page) < std::stoi(page_count);
    } catch(...) {
      return false;
    }
  }

  return false;
}

void ingest_endpoint(
  Db& db,
  const Config& config,
  const std::string& source_system,
  const std::string& base,
  const std::string& endpoint
)
{
  const int limit = 100;
  const int max_pages = 50;

  for(int page = 1; page <= max_pages; ++page) {
    std::string url = endpoint_url(source_system, base, endpoint, page, limit);

    try {
      auto response = http_get(url);

      if(response.status == 404) {
        if(page == 1) {
          log("INFO", "Governance endpoint not available: " + url);
        }

        break;
      }

      if(response.status >= 400) {
        log_fetch_error(db, config, source_system, url, response.status, "HTTP error");
        break;
      }

      JsonValue root = JsonParser(response.body).parse();
      auto records = find_candidate_records(root);

      size_t written = 0;

      for(const JsonValue* record : records) {
        GovApiDocument doc = parse_record(
          source_system,
          endpoint,
          url,
          response,
          *record
        );

        if(doc.raw_text.empty() && doc.title.empty()) {
          continue;
        }

        upsert_document(db, config, doc);
        ++written;
      }

      log(
        "INFO",
        "Governance API fetched " + source_system +
        " endpoint=" + endpoint +
        " page=" + std::to_string(page) +
        " records=" + std::to_string(written)
      );

      if(!response_has_more(root, records.size(), limit)) {
        break;
      }
    } catch(const std::exception& e) {
      log_fetch_error(db, config, source_system, url, 0, e.what());
      log("WARN", "Governance API fetch failed for " + url + ": " + e.what());
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(config.gov_pause_ms));
  }
}

} // namespace

void sync_governance(Db& db, const Config& config)
{
  log("INFO", "Starting governance API sync");

  log(
    "INFO",
    "Governance outcomes API base: " + config.gov_outcomes_api_base
  );

  log(
    "INFO",
    "Governance proposal pillar API base: " + config.gov_proposal_pillar_api_base
  );

  for(const auto& endpoint : config.gov_outcomes_endpoints) {
    ingest_endpoint(
      db,
      config,
      "outcomes",
      config.gov_outcomes_api_base,
      endpoint
    );
  }

  for(const auto& endpoint : config.gov_proposal_pillar_endpoints) {
    ingest_endpoint(
      db,
      config,
      "proposal_pillar",
      config.gov_proposal_pillar_api_base,
      endpoint
    );
  }

  log("INFO", "Finished governance API sync");
}

} // namespace cap
