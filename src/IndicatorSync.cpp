#include "cap/IndicatorSync.hpp"

#include "cap/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cap {
namespace {

struct Candle {
  std::string ts;
  double high{};
  double low{};
  double close{};
};

std::string decimal(double value)
{
  if(!std::isfinite(value)) {
    throw std::runtime_error("non-finite indicator value");
  }

  std::ostringstream stream;
  stream << std::fixed << std::setprecision(18) << value;
  return stream.str();
}

std::string checkpoint_entity(
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source
)
{
  return asset_id + ":" + interval + ":" + ohlcv_source;
}

std::string get_checkpoint(
  Db& db,
  const std::string& entity,
  const std::string& default_value
)
{
  return db.scalar(
    "SELECT checkpoint_value FROM etl_checkpoint "
    "WHERE source='indicators' "
    "AND entity_id='" + shell_escape(entity) + "' "
    "AND checkpoint_key='last_ts'",
    default_value
  );
}

void upsert_checkpoint(Db& db, const std::string& entity, const std::string& ts)
{
  db.exec(
    "INSERT INTO etl_checkpoint(source,entity_id,checkpoint_key,checkpoint_value) "
    "VALUES('indicators','" + shell_escape(entity) + "','last_ts','" +
    shell_escape(ts) + "') "
    "ON CONFLICT(source,entity_id,checkpoint_key) "
    "DO UPDATE SET checkpoint_value=EXCLUDED.checkpoint_value, updated_at=now()"
  );
}

std::vector<Candle> load_candles(
  Db& db,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::string& checkpoint,
  int warmup_candles
)
{
  std::string where =
    "WHERE asset_id='" + shell_escape(asset_id) + "' "
    "AND interval='" + shell_escape(interval) + "' "
    "AND source='" + shell_escape(ohlcv_source) + "' ";

  if(!checkpoint.empty()) {
    where +=
      "AND ts >= ('" + shell_escape(checkpoint) +
      "'::timestamptz - interval '" + std::to_string(warmup_candles) +
      " hours') ";
  }

  auto rows = db.rows(
    "SELECT "
    "to_char(ts AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),"
    "high::text,"
    "low::text,"
    "close::text "
    "FROM asset_ohlcv " + where +
    "ORDER BY ts ASC"
  );

  std::vector<Candle> candles;

  for(const auto& row : rows) {
    candles.push_back({
      row[0],
      std::stod(row[1]),
      std::stod(row[2]),
      std::stod(row[3])
    });
  }

  return candles;
}

bool should_write(const std::string& candle_ts, const std::string& checkpoint)
{
  // Recompute the checkpoint candle too.
  // This is necessary because the latest 1h candle may already exist
  // in asset_ohlcv as provisional and then get overwritten with the
  // official final OHLCV values after the hour closes.
  return checkpoint.empty() || candle_ts >= checkpoint;
}

void upsert_indicator(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::string& ts,
  const std::string& indicator,
  int period,
  double value,
  const std::string& params_json
)
{
  db.exec(
    "INSERT INTO asset_indicator("
    "asset_id,ts,interval,ohlcv_source,indicator,period,value,params,source"
    ") VALUES("
    "'" + shell_escape(asset_id) + "',"
    "'" + shell_escape(ts) + "'::timestamptz,"
    "'" + shell_escape(interval) + "',"
    "'" + shell_escape(ohlcv_source) + "',"
    "'" + shell_escape(indicator) + "',"
    + std::to_string(period) + ","
    + decimal(value) + ","
    "'" + shell_escape(params_json) + "'::jsonb,"
    "'" + shell_escape(config.indicator_source) + "'"
    ") "
    "ON CONFLICT(asset_id,ts,interval,ohlcv_source,indicator,period,params,source) "
    "DO UPDATE SET value=EXCLUDED.value, created_at=now()"
  );
}

void compute_sma(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::vector<Candle>& candles,
  const std::string& checkpoint,
  int period
)
{
  if(period <= 0 || candles.size() < static_cast<size_t>(period)) {
    return;
  }

  double sum = 0.0;

  for(size_t i = 0; i < candles.size(); ++i) {
    sum += candles[i].close;

    if(i >= static_cast<size_t>(period)) {
      sum -= candles[i - period].close;
    }

    if(i + 1 >= static_cast<size_t>(period) && should_write(candles[i].ts, checkpoint)) {
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "sma", period, sum / period, "{}"
      );
    }
  }
}

void compute_ema(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::vector<Candle>& candles,
  const std::string& checkpoint,
  int period
)
{
  if(period <= 0 || candles.empty()) {
    return;
  }

  const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
  double ema = candles.front().close;

  for(size_t i = 0; i < candles.size(); ++i) {
    if(i == 0) {
      ema = candles[i].close;
    } else {
      ema = (candles[i].close * alpha) + (ema * (1.0 - alpha));
    }

    if(i + 1 >= static_cast<size_t>(period) && should_write(candles[i].ts, checkpoint)) {
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "ema", period, ema, "{}"
      );
    }
  }
}

void compute_rsi(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::vector<Candle>& candles,
  const std::string& checkpoint,
  int period
)
{
  if(period <= 0 || candles.size() <= static_cast<size_t>(period)) {
    return;
  }

  double avg_gain = 0.0;
  double avg_loss = 0.0;

  for(int i = 1; i <= period; ++i) {
    double change = candles[i].close - candles[i - 1].close;
    avg_gain += std::max(change, 0.0);
    avg_loss += std::max(-change, 0.0);
  }

  avg_gain /= period;
  avg_loss /= period;

  for(size_t i = period; i < candles.size(); ++i) {
    if(i > static_cast<size_t>(period)) {
      double change = candles[i].close - candles[i - 1].close;
      double gain = std::max(change, 0.0);
      double loss = std::max(-change, 0.0);

      avg_gain = ((avg_gain * (period - 1)) + gain) / period;
      avg_loss = ((avg_loss * (period - 1)) + loss) / period;
    }

    double rsi = avg_loss == 0.0
      ? 100.0
      : 100.0 - (100.0 / (1.0 + (avg_gain / avg_loss)));

    if(should_write(candles[i].ts, checkpoint)) {
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "rsi", period, rsi, "{}"
      );
    }
  }
}

void compute_bollinger(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::vector<Candle>& candles,
  const std::string& checkpoint
)
{
  const int period = config.bb_period;

  if(period <= 0 || candles.size() < static_cast<size_t>(period)) {
    return;
  }

  const std::string params =
    "{\"stddev\":" + decimal(config.bb_stddev) + "}";

  for(size_t i = period - 1; i < candles.size(); ++i) {
    double sum = 0.0;

    for(size_t j = i + 1 - period; j <= i; ++j) {
      sum += candles[j].close;
    }

    double middle = sum / period;
    double variance = 0.0;

    for(size_t j = i + 1 - period; j <= i; ++j) {
      double delta = candles[j].close - middle;
      variance += delta * delta;
    }

    double stddev = std::sqrt(variance / period);
    double upper = middle + config.bb_stddev * stddev;
    double lower = middle - config.bb_stddev * stddev;

    if(should_write(candles[i].ts, checkpoint)) {
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "bb_middle", period, middle, params
      );
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "bb_upper", period, upper, params
      );
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "bb_lower", period, lower, params
      );
    }
  }
}

std::vector<double> ema_series(const std::vector<Candle>& candles, int period)
{
  std::vector<double> output(candles.size(), std::numeric_limits<double>::quiet_NaN());

  if(period <= 0 || candles.empty()) {
    return output;
  }

  double alpha = 2.0 / (period + 1.0);
  double ema = candles.front().close;

  for(size_t i = 0; i < candles.size(); ++i) {
    if(i == 0) {
      ema = candles[i].close;
    } else {
      ema = candles[i].close * alpha + ema * (1.0 - alpha);
    }

    if(i + 1 >= static_cast<size_t>(period)) {
      output[i] = ema;
    }
  }

  return output;
}

void compute_macd(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source,
  const std::vector<Candle>& candles,
  const std::string& checkpoint
)
{
  if(!config.macd_enabled || candles.size() < 35) {
    return;
  }

  const int fast = config.macd_fast;
  const int slow = config.macd_slow;
  const int signal_period = config.macd_signal;

  auto fast_ema = ema_series(candles, fast);
  auto slow_ema = ema_series(candles, slow);

  std::vector<double> macd(candles.size(), std::numeric_limits<double>::quiet_NaN());
  std::vector<double> signal(candles.size(), std::numeric_limits<double>::quiet_NaN());

  double signal_value = 0.0;
  bool signal_seeded = false;
  int signal_count = 0;

  for(size_t i = 0; i < candles.size(); ++i) {
    if(std::isfinite(fast_ema[i]) && std::isfinite(slow_ema[i])) {
      macd[i] = fast_ema[i] - slow_ema[i];

      if(!signal_seeded) {
        signal_value += macd[i];
        ++signal_count;

        if(signal_count == signal_period) {
          signal_value /= signal_period;
          signal_seeded = true;
          signal[i] = signal_value;
        }
      } else {
        const double alpha = 2.0 / (signal_period + 1.0);
        signal_value = macd[i] * alpha + signal_value * (1.0 - alpha);
        signal[i] = signal_value;
      }
    }

    if(
      std::isfinite(macd[i]) &&
      std::isfinite(signal[i]) &&
      should_write(candles[i].ts, checkpoint)
    ) {
      const std::string params =
        "{\"fast\":" + std::to_string(fast) +
        ",\"slow\":" + std::to_string(slow) +
        ",\"signal\":" + std::to_string(signal_period) + "}";

      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "macd", fast, macd[i], params
      );
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "macd_signal", signal_period, signal[i], params
      );
      upsert_indicator(
        db, config, asset_id, interval, ohlcv_source,
        candles[i].ts, "macd_histogram", signal_period, macd[i] - signal[i], params
      );
    }
  }
}

void compute_for_asset(
  Db& db,
  const Config& config,
  const std::string& asset_id,
  const std::string& interval,
  const std::string& ohlcv_source
)
{
  const std::string entity = checkpoint_entity(asset_id, interval, ohlcv_source);
  const std::string checkpoint = get_checkpoint(db, entity, "");

  auto candles = load_candles(
    db,
    asset_id,
    interval,
    ohlcv_source,
    checkpoint,
    config.indicator_warmup_candles
  );

  if(candles.empty()) {
    return;
  }

  for(int period : config.sma_periods) {
    compute_sma(db, config, asset_id, interval, ohlcv_source, candles, checkpoint, period);
  }

  for(int period : config.ema_periods) {
    compute_ema(db, config, asset_id, interval, ohlcv_source, candles, checkpoint, period);
  }

  for(int period : config.rsi_periods) {
    compute_rsi(db, config, asset_id, interval, ohlcv_source, candles, checkpoint, period);
  }

  compute_bollinger(db, config, asset_id, interval, ohlcv_source, candles, checkpoint);
  compute_macd(db, config, asset_id, interval, ohlcv_source, candles, checkpoint);

  upsert_checkpoint(db, entity, candles.back().ts);

  log(
    "INFO",
    "Indicators synced for " + asset_id +
    " interval=" + interval +
    " ohlcv_source=" + ohlcv_source +
    " through " + candles.back().ts
  );
}

} // namespace

void sync_indicators(Db& db, const Config& config)
{
  if(!config.indicators_enabled) {
    return;
  }

  auto assets = db.rows(
    "SELECT DISTINCT asset_id, interval, source "
    "FROM asset_ohlcv "
    "WHERE interval='1h' "
    "ORDER BY asset_id, source"
  );

  log("INFO", "Indicator asset/source groups found: " + std::to_string(assets.size()));

  for(const auto& row : assets) {
    try {
      compute_for_asset(db, config, row[0], row[1], row[2]);
    } catch(const std::exception& e) {
      log(
        "ERROR",
        "Indicator sync failed for " + row[0] +
        "; will retry in next cycle: " + std::string(e.what())
      );
    }
  }
}

} // namespace cap
