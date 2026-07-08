#pragma once

#include <string>
#include <vector>

namespace cap {

struct Config {
  std::string cfg_path = "config/config.local.json";
  std::string db_host = "localhost";
  std::string db_user = "postgres";
  std::string db_pass = "mysecretpassword";
  std::string db_name = "cap";
  std::string schema_file = "/app/sql/schema.sql";
  int db_port = 5432;
  bool run_once = false;
  bool bootstrap_schema = true;
  bool ohlcv_enabled = true;
  bool gov_enabled = true;
  int loop_sleep = 60;

  std::string ohlcv_provider = "binance_spot";
  std::string ohlcv_base = "https://api.binance.com";
  std::string binance_data_base = "https://data.binance.vision";
  std::string archive_tmp_dir = "/tmp/cap-offchain-etl/binance-klines";
  bool archive_bootstrap_enabled = true;

  std::string quote = "USDT";
  std::string interval = "1h";
  std::string bootstrap_from = "2017-01-01T00:00:00Z";
  std::string asset_relationships_file = "config/asset_relationships.csv";
  std::string mapping_file = "/app/config/asset_mapping.csv";
  int request_limit = 1000;
  int request_pause_ms = 250;
  int empty_advances = 50;

  bool indicators_enabled = true;
  std::string indicator_source = "cap-offchain-etl";
  int indicator_warmup_candles = 1200;

  std::vector<int> sma_periods = {20, 50, 100, 200};
  std::vector<int> ema_periods = {9, 20, 50, 100, 200};
  std::vector<int> rsi_periods = {14};

  int bb_period = 20;
  double bb_stddev = 2.0;

  bool macd_enabled = true;
  int macd_fast = 12;
  int macd_slow = 26;
  int macd_signal = 9;

  std::string gov_provider = "govtool_public";
  std::string gov_manual = "/app/config/governance_sources.csv";
  int gov_pause_ms = 500;
  int gov_max_links = 500;
  int gov_refetch_hours = 24;
  std::vector<std::string> gov_discovery;

  std::string gov_outcomes_api_base = "https://be.outcomes.gov.tools";
  std::string gov_proposal_pillar_api_base = "https://be.pdf.gov.tools/api";

  std::vector<std::string> gov_outcomes_endpoints = {
    "/governance-actions"
  };

  std::vector<std::string> gov_proposal_pillar_endpoints = {
    "/proposals",
    "/proposal-discussions",
    "/budget-proposals",
    "/budget-discussions",
    "/governance-actions"
  };
};

Config load_config(const std::string& path);

} // namespace cap
