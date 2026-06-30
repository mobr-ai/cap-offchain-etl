#include "cap/OhlcvSync.hpp"

#include "cap/Http.hpp"
#include "cap/Utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace cap {
namespace {

long long normalize_epoch_ms(long long value)
{
  // Binance API usually returns milliseconds.
  // Some archive CSVs may contain microseconds.
  if(value > 99999999999999LL) {
    return value / 1000LL;
  }

  return value;
}


const long long HOUR_MS = 3600000LL;

long long utc_now_ms()
{
  return static_cast<long long>(time(nullptr)) * 1000LL;
}

long long hour_start_ms(long long milliseconds)
{
  return (milliseconds / HOUR_MS) * HOUR_MS;
}

struct KlineRow {
  long long open_ms{};
  std::string open;
  std::string high;
  std::string low;
  std::string close;
  std::string volume;
  bool final{};
};

struct CurrentCandleState {
  long long open_ms{};
  bool final{true};
  long long updated_at_ms{};
  std::string close;
};

std::string candle_status(bool final)
{
  return final ? "final" : "provisional";
}

std::string market_entity(const AssetMap& asset)
{
  return asset.source_market_id.empty()
    ? asset.source + ":" + asset.source_asset
    : asset.source_market_id;
}

std::string current_candle_state_file(const Config& config)
{
  std::filesystem::path directory = config.archive_tmp_dir.empty()
    ? std::filesystem::path("/tmp/cap-offchain-etl")
    : std::filesystem::path(config.archive_tmp_dir);

  return (directory / "ohlcv_current_state.csv").string();
}

std::map<std::string, CurrentCandleState> load_current_candle_state(
  const Config& config
)
{
  std::map<std::string, CurrentCandleState> output;
  std::ifstream file(current_candle_state_file(config));

  if(!file) {
    return output;
  }

  std::string line;

  while(std::getline(file, line)) {
    line = trim(line);

    if(line.empty()) {
      continue;
    }

    auto values = split_csv(line);

    if(values.size() < 5 || values[0] == "entity") {
      continue;
    }

    try {
      CurrentCandleState state;
      state.open_ms = std::stoll(values[1]);
      state.final = values[2] == "final" || values[2] == "true" || values[2] == "1";
      state.updated_at_ms = std::stoll(values[3]);
      state.close = values[4];

      output[values[0]] = state;
    } catch(...) {
      log("WARN", "Ignoring invalid OHLCV current-state line: " + line);
    }
  }

  return output;
}

void save_current_candle_state(
  const Config& config,
  const std::map<std::string, CurrentCandleState>& state
)
{
  std::filesystem::path path = current_candle_state_file(config);
  std::filesystem::create_directories(path.parent_path());

  std::filesystem::path tmp_path = path.string() + ".tmp";

  std::ofstream file(tmp_path);

  if(!file) {
    throw std::runtime_error("cannot write OHLCV current-state file: " + tmp_path.string());
  }

  file << "entity,open_time_ms,status,updated_at_ms,close\n";

  for(const auto& item : state) {
    file
      << item.first << ","
      << item.second.open_ms << ","
      << candle_status(item.second.final) << ","
      << item.second.updated_at_ms << ","
      << item.second.close << "\n";
  }

  file.close();

  std::filesystem::rename(tmp_path, path);
}


void upsert_checkpoint(
  Db& db,
  const std::string& source,
  const std::string& id,
  const std::string& key,
  const std::string& value
)
{
  db.exec(
    "INSERT INTO etl_checkpoint(source,entity_id,checkpoint_key,checkpoint_value) "
    "VALUES('" + shell_escape(source) + "','" + shell_escape(id) + "','" +
    shell_escape(key) + "','" + shell_escape(value) + "') "
    "ON CONFLICT(source,entity_id,checkpoint_key) "
    "DO UPDATE SET checkpoint_value=EXCLUDED.checkpoint_value, updated_at=now()"
  );
}

std::string get_checkpoint(
  Db& db,
  const std::string& source,
  const std::string& id,
  const std::string& key,
  const std::string& default_value
)
{
  return db.scalar(
    "SELECT checkpoint_value FROM etl_checkpoint "
    "WHERE source='" + shell_escape(source) + "' "
    "AND entity_id='" + shell_escape(id) + "' "
    "AND checkpoint_key='" + shell_escape(key) + "'",
    default_value
  );
}

bool checkpoint_exists(
  Db& db,
  const std::string& source,
  const std::string& id,
  const std::string& key
)
{
  return db.scalar(
    "SELECT count(*) FROM etl_checkpoint "
    "WHERE source='" + shell_escape(source) + "' "
    "AND entity_id='" + shell_escape(id) + "' "
    "AND checkpoint_key='" + shell_escape(key) + "'",
    "0"
  ) != "0";
}

std::string latest_ohlcv_checkpoint(Db& db, const AssetMap& asset)
{
  const std::string entity = market_entity(asset);

  std::string last_open = get_checkpoint(
    db,
    "ohlcv",
    entity,
    "last_open_time_ms",
    "0"
  );

  std::string last_empty_search = get_checkpoint(
    db,
    "ohlcv",
    entity,
    "last_empty_search_ms",
    "0"
  );

  long long last_open_ms = last_open.empty() ? 0 : std::stoll(last_open);
  long long last_empty_search_ms =
    last_empty_search.empty() ? 0 : std::stoll(last_empty_search);

  // Do not let a temporary empty response for the live/current hour
  // move the effective checkpoint beyond the active candle.
  long long current_hour_open_ms = hour_start_ms(utc_now_ms());

  if(last_empty_search_ms >= current_hour_open_ms) {
    last_empty_search_ms = 0;
  }

  long long latest_attempt_ms = std::max(last_open_ms, last_empty_search_ms);

  return latest_attempt_ms == 0 ? "" : std::to_string(latest_attempt_ms);
}

long long effective_bootstrap_ms(const Config& config, const AssetMap& asset)
{
  long long config_bootstrap_ms = parse_utc_ms(config.bootstrap_from);

  long long asset_bootstrap_ms = asset.bootstrap.empty()
    ? config_bootstrap_ms
    : parse_utc_ms(asset.bootstrap);

  long long valid_from_ms = asset.valid_from.empty()
    ? asset_bootstrap_ms
    : parse_utc_ms(asset.valid_from);

  return std::max({config_bootstrap_ms, asset_bootstrap_ms, valid_from_ms});
}

long long effective_end_ms(const AssetMap& asset, long long fallback_end_ms)
{
  if(asset.valid_to.empty()) {
    return fallback_end_ms;
  }

  return std::min(fallback_end_ms, parse_utc_ms(asset.valid_to) - 1);
}

void upsert_asset_source(Db& db, const AssetMap& asset)
{
  std::string source_market_id = asset.source_market_id.empty()
    ? asset.source + ":" + asset.source_asset
    : asset.source_market_id;

  std::string valid_from = asset.valid_from.empty()
    ? asset.bootstrap
    : asset.valid_from;

  std::string valid_to_sql = asset.valid_to.empty()
    ? "NULL"
    : "'" + shell_escape(asset.valid_to) + "'::timestamptz";

  std::string sql =
    "INSERT INTO asset(asset_id,symbol,name,policy_id,asset_name_hex,decimals) "
    "VALUES('" + shell_escape(asset.asset_id) + "','" +
    shell_escape(asset.symbol) + "','" +
    shell_escape(asset.name) + "','" +
    shell_escape(asset.policy) + "','" +
    shell_escape(asset.hex) + "'," + asset.decimals + ") "
    "ON CONFLICT(asset_id) DO UPDATE SET "
    "symbol=EXCLUDED.symbol,"
    "name=EXCLUDED.name,"
    "policy_id=EXCLUDED.policy_id,"
    "asset_name_hex=EXCLUDED.asset_name_hex,"
    "decimals=EXCLUDED.decimals";

  db.exec(sql);

  sql =
    "INSERT INTO asset_market_source("
    "asset_id,source,source_asset_id,quote_asset,base_asset_symbol,"
    "source_market_id,valid_from,valid_to,bootstrap_from"
    ") VALUES('" +
    shell_escape(asset.asset_id) + "','" +
    shell_escape(asset.source) + "','" +
    shell_escape(asset.source_asset) + "','" +
    shell_escape(asset.quote) + "','" +
    shell_escape(asset.base_asset_symbol) + "','" +
    shell_escape(source_market_id) + "','" +
    shell_escape(valid_from) + "'::timestamptz," +
    valid_to_sql + ",'" +
    shell_escape(asset.bootstrap) + "'::timestamptz) "
    "ON CONFLICT(source,source_market_id) DO UPDATE SET "
    "asset_id=EXCLUDED.asset_id,"
    "source_asset_id=EXCLUDED.source_asset_id,"
    "quote_asset=EXCLUDED.quote_asset,"
    "base_asset_symbol=EXCLUDED.base_asset_symbol,"
    "valid_from=EXCLUDED.valid_from,"
    "valid_to=EXCLUDED.valid_to,"
    "bootstrap_from=EXCLUDED.bootstrap_from,"
    "enabled=true";

  db.exec(sql);
}

std::string market_source_db_id(Db& db, const AssetMap& asset)
{
  std::string source_market_id = asset.source_market_id.empty()
    ? asset.source + ":" + asset.source_asset
    : asset.source_market_id;

  return db.scalar(
    "SELECT id::text FROM asset_market_source "
    "WHERE source='" + shell_escape(asset.source) + "' "
    "AND source_market_id='" + shell_escape(source_market_id) + "'",
    ""
  );
}

std::vector<KlineRow> parse_klines(const std::string& body, long long now_ms)
{
  std::vector<KlineRow> output;

  std::regex row_regex(
    "\\[([0-9]+),\"([^\"]+)\",\"([^\"]+)\",\"([^\"]+)\",\"([^\"]+)\",\"([^\"]+)\",([0-9]+)"
  );

  for(
    std::sregex_iterator it(body.begin(), body.end(), row_regex), end;
    it != end;
    ++it
  ) {
    long long open_ms = normalize_epoch_ms(std::stoll((*it)[1].str()));
    long long close_ms = normalize_epoch_ms(std::stoll((*it)[7].str()));

    output.push_back({
      open_ms,
      (*it)[2].str(),
      (*it)[3].str(),
      (*it)[4].str(),
      (*it)[5].str(),
      (*it)[6].str(),
      now_ms > close_ms
    });
  }

  return output;
}

std::vector<KlineRow> parse_binance_kline_csv(
  const std::string& csv
)
{
  std::vector<KlineRow> output;
  std::stringstream stream(csv);
  std::string line;

  while(std::getline(stream, line)) {
    line = trim(line);

    if(line.empty()) {
      continue;
    }

    auto values = split_csv(line);

    if(values.size() < 6) {
      continue;
    }

    if(values[0].empty() || !std::isdigit(static_cast<unsigned char>(values[0][0]))) {
      continue;
    }

    output.push_back({
      normalize_epoch_ms(std::stoll(values[0])),
      values[1],
      values[2],
      values[3],
      values[4],
      values[5],
      true
    });
  }

  return output;
}

std::vector<std::string> generate_binance_daily_kline_archive_keys(
  const Config& config,
  const std::string& symbol,
  long long start_ms,
  long long end_ms
)
{
  if(!safe_market_token(symbol)) {
    throw std::runtime_error("unsafe Binance symbol: " + symbol);
  }

  if(config.interval != "1h") {
    throw std::runtime_error(
      "Binance archive bootstrap currently supports only 1h candles"
    );
  }

  std::vector<std::string> keys;
  const long long day_ms = 86400000LL;

  time_t start_seconds = static_cast<time_t>(start_ms / 1000);
  tm start_time{};
  gmtime_r(&start_seconds, &start_time);
  start_time.tm_hour = 0;
  start_time.tm_min = 0;
  start_time.tm_sec = 0;

  long long day_start_ms = static_cast<long long>(timegm(&start_time)) * 1000LL;

  for(long long t = day_start_ms; t <= end_ms; t += day_ms) {
    time_t seconds = static_cast<time_t>(t / 1000);
    tm day{};
    gmtime_r(&seconds, &day);

    char date_buffer[16];
    strftime(date_buffer, sizeof(date_buffer), "%Y-%m-%d", &day);

    std::string file = symbol + "-" + config.interval + "-" +
      std::string(date_buffer) + ".zip";

    keys.push_back(
      "data/spot/daily/klines/" + symbol + "/" + config.interval + "/" + file
    );
  }

  return keys;
}

void upsert_ohlcv_rows(
  Db& db,
  const AssetMap& asset,
  const std::vector<KlineRow>& rows,
  const Config* config = nullptr,
  std::map<std::string, CurrentCandleState>* current_state = nullptr
)
{
  if(rows.empty()) {
    return;
  }

  const std::string market_id = market_source_db_id(db, asset);

  if(market_id.empty()) {
    throw std::runtime_error(
      "asset_market_source not found for " + asset.source + ":" + asset.source_asset
    );
  }

  long long max_final_open_ms = 0;

  for(const auto& row : rows) {
    if(row.final) {
      max_final_open_ms = std::max(max_final_open_ms, row.open_ms);
    }

    db.exec(
      "INSERT INTO asset_ohlcv("
      "asset_id,market_source_id,ts,interval,open,high,low,close,volume,"
      "source,source_asset_id,quote_asset"
      ") VALUES("
      "'" + shell_escape(asset.asset_id) + "'," +
      market_id + ","
      "to_timestamp(" + std::to_string(row.open_ms / 1000) + "),"
      "'1h'," +
      row.open + "," +
      row.high + "," +
      row.low + "," +
      row.close + "," +
      row.volume + ","
      "'" + shell_escape(asset.source) + "',"
      "'" + shell_escape(asset.source_asset) + "',"
      "'" + shell_escape(asset.quote) + "'"
      ") "
      "ON CONFLICT(market_source_id,ts,interval) DO UPDATE SET "
      "open=EXCLUDED.open,"
      "high=EXCLUDED.high,"
      "low=EXCLUDED.low,"
      "close=EXCLUDED.close,"
      "volume=EXCLUDED.volume,"
      "asset_id=EXCLUDED.asset_id,"
      "source=EXCLUDED.source,"
      "source_asset_id=EXCLUDED.source_asset_id,"
      "quote_asset=EXCLUDED.quote_asset"
    );
  }

  const std::string entity = market_entity(asset);

  if(max_final_open_ms > 0) {
    std::string current_checkpoint = get_checkpoint(
      db,
      "ohlcv",
      entity,
      "last_open_time_ms",
      "0"
    );

    long long current_ms = std::stoll(current_checkpoint);

    if(max_final_open_ms > current_ms) {
      upsert_checkpoint(
        db,
        "ohlcv",
        entity,
        "last_open_time_ms",
        std::to_string(max_final_open_ms)
      );
    }
  }

  if(config && current_state) {
    const KlineRow& latest = rows.back();

    (*current_state)[entity] = {
      latest.open_ms,
      latest.final,
      utc_now_ms(),
      latest.close
    };

    save_current_candle_state(*config, *current_state);

    log(
      "INFO",
      "OHLCV current-state " + entity +
      " ts=" + ms_to_iso(latest.open_ms) +
      " status=" + candle_status(latest.final) +
      " close=" + latest.close
    );
  }
}

void sync_binance_archives(Db& db, const Config& config, const AssetMap& asset)
{
  if(!config.archive_bootstrap_enabled) {
    return;
  }

  if(asset.source != "binance_spot") {
    return;
  }

  const std::string entity = asset.source_market_id.empty()
    ? asset.source + ":" + asset.source_asset
    : asset.source_market_id;

  const long long bootstrap_ms = effective_bootstrap_ms(config, asset);

  long long now_ms = static_cast<long long>(time(nullptr)) * 1000LL;
  time_t now_seconds = static_cast<time_t>(now_ms / 1000);
  tm utc_now{};

  gmtime_r(&now_seconds, &utc_now);
  utc_now.tm_hour = 0;
  utc_now.tm_min = 0;
  utc_now.tm_sec = 0;

  long long today_midnight_ms =
    static_cast<long long>(timegm(&utc_now)) * 1000LL;

  long long archive_end_ms = effective_end_ms(asset, today_midnight_ms - 1);

  if(archive_end_ms < bootstrap_ms) {
    log(
      "INFO",
      "Skipping Binance archives for inactive market " + asset.source_asset
    );
    return;
  }

  auto keys = generate_binance_daily_kline_archive_keys(
    config,
    asset.source_asset,
    bootstrap_ms,
    archive_end_ms
  );

  log(
    "INFO",
    "Binance archive files generated for " + asset.source_asset + ": " +
    std::to_string(keys.size())
  );

  for(const auto& key : keys) {
    const std::string checkpoint_key = "archive_zip:" + key;

    if(checkpoint_exists(db, "ohlcv", entity, checkpoint_key)) {
      continue;
    }

    std::string url = config.binance_data_base + "/" + key;
    std::string local_zip = config.archive_tmp_dir + "/" + asset.source_asset +
      "/" + std::filesystem::path(key).filename().string();

    try {
      http_download_file(url, local_zip);

      std::string csv = run_command_capture(
        "unzip -p '" + shell_escape(local_zip) + "'"
      );

      auto rows = parse_binance_kline_csv(csv);
      std::vector<KlineRow> filtered;

      for(const auto& row : rows) {
        if(row.open_ms >= bootstrap_ms && row.open_ms <= archive_end_ms) {
          filtered.push_back(row);
        }
      }

      if(!filtered.empty()) {
        upsert_ohlcv_rows(db, asset, filtered);
        log(
          "INFO",
          "Loaded Binance archive " + key + " candles=" +
          std::to_string(filtered.size())
        );
      } else {
        log("INFO", "Skipped Binance archive outside valid market range: " + key);
      }

      upsert_checkpoint(db, "ohlcv", entity, checkpoint_key, "processed");
      std::filesystem::remove(local_zip);
    } catch(const std::exception& e) {
      const std::string error = e.what();

      if(error.find("HTTP 404 for ") != std::string::npos) {
        upsert_checkpoint(db, "ohlcv", entity, checkpoint_key, "missing_404");
        log(
          "WARN",
          "Binance archive missing for " + key +
          "; marking as missing_404 and skipping in later cycles"
        );
      } else {
        log(
          "WARN",
          "Binance archive unavailable for " + key +
          "; API sync will backfill missing candles automatically: " +
          error
        );
      }

      std::filesystem::remove(local_zip);
      continue;
    }

    std::this_thread::sleep_for(
      std::chrono::milliseconds(config.request_pause_ms)
    );
  }
}

} // namespace

std::vector<AssetMap> load_assets(const std::string& file)
{
  std::vector<AssetMap> output;
  std::ifstream stream(file);

  if(!stream) {
    throw std::runtime_error("cannot open asset mapping file: " + file);
  }

  std::string line;
  bool first = true;

  while(std::getline(stream, line)) {
    line = trim(line);

    if(line.empty() || line[0] == '#') {
      continue;
    }

    auto values = split_csv(line);

    if(first && !values.empty() && values[0] == "asset_id") {
      first = false;
      continue;
    }

    first = false;

    if(values.size() < 14) {
      throw std::runtime_error("invalid asset mapping line: " + line);
    }

    std::string source_market_id = values[13].empty()
      ? values[6] + ":" + values[7]
      : values[13];

    output.push_back({
      values[0],
      values[1],
      values[2],
      values[3],
      values[4],
      values[5],
      values[6],
      values[7],
      values[8],
      values[9],
      values[10],
      values[11],
      values[12],
      source_market_id
    });
  }

  return output;
}

std::vector<AssetRelationshipMap> load_asset_relationships(const std::string& file)
{
  std::vector<AssetRelationshipMap> output;
  std::ifstream stream(file);

  if(!stream) {
    log("WARN", "asset relationship file not found: " + file);
    return output;
  }

  std::string line;
  bool first = true;

  while(std::getline(stream, line)) {
    line = trim(line);

    if(line.empty() || line[0] == '#') {
      continue;
    }

    auto values = split_csv(line);

    if(first && !values.empty() && values[0] == "from_asset_id") {
      first = false;
      continue;
    }

    first = false;

    if(values.size() < 5) {
      throw std::runtime_error("invalid asset relationship line: " + line);
    }

    output.push_back({
      values[0],
      values[1],
      values[2],
      values[3],
      values[4]
    });
  }

  return output;
}

void upsert_asset_relationships(Db& db, const std::vector<AssetRelationshipMap>& relationships)
{
  for(const auto& relationship : relationships) {
    std::string effective_at_sql = relationship.effective_at.empty()
      ? "NULL"
      : "'" + shell_escape(relationship.effective_at) + "'::timestamptz";

    std::string metadata = relationship.metadata.empty()
      ? "{}"
      : relationship.metadata;

    db.exec(
      "INSERT INTO asset_relationship("
      "from_asset_id,to_asset_id,relationship_type,effective_at,metadata"
      ") VALUES('" +
      shell_escape(relationship.from_asset_id) + "','" +
      shell_escape(relationship.to_asset_id) + "','" +
      shell_escape(relationship.relationship_type) + "'," +
      effective_at_sql + ",'" +
      shell_escape(metadata) + "'::jsonb) "
      "ON CONFLICT(from_asset_id,to_asset_id,relationship_type) "
      "DO UPDATE SET "
      "effective_at=EXCLUDED.effective_at,"
      "metadata=EXCLUDED.metadata"
    );
  }
}

void sync_ohlcv(Db& db, const Config& config)
{
  auto assets = load_assets(config.mapping_file);
  log("INFO", "OHLCV mappings loaded: " + std::to_string(assets.size()));

  for(const auto& asset : assets) {
    upsert_asset_source(db, asset);
  }

  upsert_asset_relationships(
    db,
    load_asset_relationships(config.asset_relationships_file)
  );

  CURL* encoder_curl = curl_easy_init();

  if(!encoder_curl) {
    throw std::runtime_error("curl init failed for OHLCV URL encoding");
  }

  auto current_state = load_current_candle_state(config);
  for(const auto& asset : assets) {
    try {
      sync_binance_archives(db, config, asset);

      const long long bootstrap_ms = effective_bootstrap_ms(config, asset);
      const long long valid_end_ms = effective_end_ms(
        asset,
        static_cast<long long>(time(nullptr)) * 1000LL
      );

      if(valid_end_ms < bootstrap_ms) {
        log("INFO", "Skipping inactive market " + asset.source_asset);
        continue;
      }

      const std::string entity = market_entity(asset);

      std::string checkpoint = latest_ohlcv_checkpoint(db, asset);
      long long start = bootstrap_ms;

      if(!checkpoint.empty() && checkpoint != "0") {
        start = std::max(bootstrap_ms, std::stoll(checkpoint) + HOUR_MS);
      }

      // If the previous run stopped while a candle was provisional,
      // resume from that same candle so it can be refreshed or finalized.
      auto state_it = current_state.find(entity);

      if(
        state_it != current_state.end() &&
        !state_it->second.final &&
        state_it->second.open_ms >= bootstrap_ms
      ) {
        start = std::min(start, state_it->second.open_ms);
      }

      long long now_ms = utc_now_ms();
      long long sync_until_ms = std::min(now_ms, valid_end_ms);
      int empty_count = 0;

      // Fetch through the active candle. The last returned candle may be provisional.
      while(start <= sync_until_ms) {
        long long end = start +
          static_cast<long long>(config.request_limit - 1) * HOUR_MS;

        if(end > sync_until_ms) {
          end = sync_until_ms;
        }

        std::string url = config.ohlcv_base +
          "/api/v3/klines?symbol=" + url_encode(encoder_curl, asset.source_asset) +
          "&interval=1h&startTime=" + std::to_string(start) +
          "&endTime=" + std::to_string(end) +
          "&limit=" + std::to_string(config.request_limit);

        auto response = http_get(url);

        if(response.status >= 400) {
          log(
            "WARN",
            "OHLCV provider returned HTTP " + std::to_string(response.status) +
            " for " + asset.source_asset +
            "; keeping checkpoint unchanged and retrying in a later cycle"
          );
          break;
        }

        long long response_now_ms = utc_now_ms();
        auto rows = parse_klines(response.body, response_now_ms);

        if(rows.empty()) {
          log(
            "WARN",
            "No API candles for " + asset.source_asset +
            " between " + ms_to_iso(start) +
            " and " + ms_to_iso(end) +
            "; advancing search window"
          );

          long long current_hour_open_ms = hour_start_ms(response_now_ms);

          // Only checkpoint empty historical windows.
          // Never checkpoint an empty live/current-hour window, otherwise the app
          // could skip the active candle after a temporary provider issue.
          if(end < current_hour_open_ms) {
            upsert_checkpoint(
              db,
              "ohlcv",
              entity,
              "last_empty_search_ms",
              std::to_string(end)
            );
          } else {
            log(
              "WARN",
              "Keeping empty-search checkpoint unchanged for live window " +
              asset.source_asset
            );
          }

          start = end + HOUR_MS;
          ++empty_count;

          if(empty_count >= config.empty_advances) {
            log(
              "WARN",
              "Reached maximum empty-window advances for " + asset.source_asset +
              "; retrying remaining history in a later cycle"
            );
            break;
          }

          std::this_thread::sleep_for(
            std::chrono::milliseconds(config.request_pause_ms)
          );
          continue;
        }

        empty_count = 0;

        upsert_ohlcv_rows(
          db,
          asset,
          rows,
          &config,
          &current_state
        );

        const KlineRow& latest = rows.back();

        log(
          "INFO",
          "OHLCV API synced " + asset.source_asset +
          " candles=" + std::to_string(rows.size()) +
          " through " + ms_to_iso(latest.open_ms) +
          " status=" + candle_status(latest.final)
        );

        start = latest.open_ms + HOUR_MS;

        std::this_thread::sleep_for(
          std::chrono::milliseconds(config.request_pause_ms)
        );
      }
    } catch(const std::exception& e) {
      log(
        "ERROR",
        "OHLCV sync failed for " + asset.source_asset +
        "; checkpoint unchanged, will retry in the next cycle: " +
        std::string(e.what())
      );
    }
  }

  curl_easy_cleanup(encoder_curl);
}

} // namespace cap
