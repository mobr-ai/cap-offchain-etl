#include <sstream>

#include "cap/Config.hpp"

#include "cap/Json.hpp"
#include "cap/Utils.hpp"

namespace cap {

std::vector<std::string> parse_string_array(
  const JsonValue& root,
  std::initializer_list<const char*> path,
  const std::vector<std::string>& fallback
)
{
  const JsonValue* value = json_at(root, path);

  if(!value || value->type != JsonValue::Type::Array) {
    return fallback;
  }

  std::vector<std::string> output;

  for(const auto& item : value->array) {
    if(item.type == JsonValue::Type::String && !trim(item.str).empty()) {
      output.push_back(trim(item.str));
    }
  }

  return output.empty() ? fallback : output;
}


std::vector<int> parse_int_list(const std::string& value, const std::vector<int>& fallback)
{
  if(value.empty()) {
    return fallback;
  }

  std::vector<int> output;
  std::stringstream stream(value);
  std::string item;

  while(std::getline(stream, item, ',')) {
    item = trim(item);

    if(!item.empty()) {
      output.push_back(std::stoi(item));
    }
  }

  return output.empty() ? fallback : output;
}


Config load_config(const std::string& path)
{
  std::string text = read_file(path);
  JsonValue root = JsonParser(text).parse();

  Config config;
  config.cfg_path = path;

  config.db_host = json_string_at(root, {"database", "host"}, config.db_host);
  config.db_port = json_int_at(root, {"database", "port"}, config.db_port);
  config.db_user = json_string_at(root, {"database", "user"}, config.db_user);
  config.db_pass = json_string_at(root, {"database", "password"}, config.db_pass);
  config.db_name = json_string_at(root, {"database", "database"}, config.db_name);
  config.schema_file = json_string_at(root, {"database", "schema_file"}, config.schema_file);

  config.run_once = json_bool_at(root, {"runtime", "run_once"}, config.run_once);
  config.bootstrap_schema = json_bool_at(
    root,
    {"runtime", "bootstrap_schema"},
    config.bootstrap_schema
  );
  config.loop_sleep = json_int_at(
    root,
    {"runtime", "loop_sleep_seconds"},
    config.loop_sleep
  );

  config.ohlcv_enabled = json_bool_at(root, {"ohlcv", "enabled"}, config.ohlcv_enabled);
  config.ohlcv_provider = json_string_at(root, {"ohlcv", "provider"}, config.ohlcv_provider);
  config.ohlcv_base = json_string_at(root, {"ohlcv", "base_url"}, config.ohlcv_base);
  config.binance_data_base = json_string_at(
    root,
    {"ohlcv", "binance_data_base_url"},
    config.binance_data_base
  );
  config.archive_tmp_dir = json_string_at(
    root,
    {"ohlcv", "archive_tmp_dir"},
    config.archive_tmp_dir
  );
  config.archive_bootstrap_enabled = json_bool_at(
    root,
    {"ohlcv", "archive_bootstrap_enabled"},
    config.archive_bootstrap_enabled
  );
  config.quote = json_string_at(root, {"ohlcv", "quote_asset"}, config.quote);
  config.interval = json_string_at(root, {"ohlcv", "interval"}, config.interval);
  config.bootstrap_from = json_string_at(root, {"ohlcv", "bootstrap_from"}, config.bootstrap_from);
  config.request_limit = json_int_at(root, {"ohlcv", "request_limit"}, config.request_limit);
  config.request_pause_ms = json_int_at(
    root,
    {"ohlcv", "request_pause_ms"},
    config.request_pause_ms
  );
  config.empty_advances = json_int_at(
    root,
    {"ohlcv", "empty_window_max_advances_per_cycle"},
    config.empty_advances
  );
  config.asset_relationships_file = json_string_at(
    root,
    {"ohlcv", "asset_relationships_file"},
    config.asset_relationships_file
  );
  config.mapping_file = json_string_at(root, {"ohlcv", "mapping_file"}, config.mapping_file);



  config.indicators_enabled = json_bool_at(
    root,
    {"indicators", "enabled"},
    config.indicators_enabled
  );

  config.indicator_source = json_string_at(
    root,
    {"indicators", "source"},
    config.indicator_source
  );

  config.indicator_warmup_candles = json_int_at(
    root,
    {"indicators", "warmup_candles"},
    config.indicator_warmup_candles
  );

  config.sma_periods = parse_int_list(
    json_string_at(root, {"indicators", "sma_periods"}, ""),
    config.sma_periods
  );

  config.ema_periods = parse_int_list(
    json_string_at(root, {"indicators", "ema_periods"}, ""),
    config.ema_periods
  );

  config.rsi_periods = parse_int_list(
    json_string_at(root, {"indicators", "rsi_periods"}, ""),
    config.rsi_periods
  );

  config.bb_period = json_int_at(
    root,
    {"indicators", "bb_period"},
    config.bb_period
  );

  config.bb_stddev = std::stod(
    json_string_at(
      root,
      {"indicators", "bb_stddev"},
      std::to_string(config.bb_stddev)
    )
  );

  config.macd_enabled = json_bool_at(
    root,
    {"indicators", "macd_enabled"},
    config.macd_enabled
  );

  config.macd_fast = json_int_at(
    root,
    {"indicators", "macd_fast"},
    config.macd_fast
  );

  config.macd_slow = json_int_at(
    root,
    {"indicators", "macd_slow"},
    config.macd_slow
  );

  config.macd_signal = json_int_at(
    root,
    {"indicators", "macd_signal"},
    config.macd_signal
  );


  config.gov_enabled = json_bool_at(root, {"governance", "enabled"}, config.gov_enabled);
  config.gov_provider = json_string_at(
    root,
    {"governance", "provider"},
    config.gov_provider
  );
  config.gov_manual = json_string_at(
    root,
    {"governance", "manual_source_file"},
    config.gov_manual
  );
  config.gov_pause_ms = json_int_at(
    root,
    {"governance", "request_pause_ms"},
    config.gov_pause_ms
  );
  config.gov_max_links = json_int_at(
    root,
    {"governance", "max_discovered_links_per_cycle"},
    config.gov_max_links
  );
  config.gov_refetch_hours = json_int_at(
    root,
    {"governance", "refetch_after_hours"},
    config.gov_refetch_hours
  );
  config.gov_discovery = json_array_strings_at(root, {"governance", "discovery_urls"});

  if(config.gov_discovery.empty()) {
    config.gov_discovery = {
      "https://gov.tools/proposal_discussion",
      "https://gov.tools/budget_discussion",
      "https://gov.tools/outcomes/governance_actions"
    };
  }

  config.gov_outcomes_api_base = json_string_at(
    root,
    {"governance", "outcomes_api_base_url"},
    config.gov_outcomes_api_base
  );

  config.gov_proposal_pillar_api_base = json_string_at(
    root,
    {"governance", "proposal_pillar_api_base_url"},
    config.gov_proposal_pillar_api_base
  );

  config.gov_outcomes_endpoints = parse_string_array(
    root,
    {"governance", "outcomes_endpoints"},
    config.gov_outcomes_endpoints
  );

  config.gov_proposal_pillar_endpoints = parse_string_array(
    root,
    {"governance", "proposal_pillar_endpoints"},
    config.gov_proposal_pillar_endpoints
  );

  return config;
}

} // namespace cap
