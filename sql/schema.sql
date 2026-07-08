CREATE TABLE IF NOT EXISTS asset (
  id BIGSERIAL PRIMARY KEY,
  asset_id TEXT NOT NULL UNIQUE,
  symbol TEXT NOT NULL,
  name TEXT,
  policy_id TEXT,
  asset_name_hex TEXT,
  decimals INTEGER NOT NULL DEFAULT 0,
  asset_type TEXT NOT NULL DEFAULT 'token',
  created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS ix_asset_symbol ON asset(symbol);
CREATE INDEX IF NOT EXISTS ix_asset_policy_id ON asset(policy_id);

CREATE TABLE IF NOT EXISTS asset_relationship (
  id BIGSERIAL PRIMARY KEY,
  from_asset_id TEXT NOT NULL REFERENCES asset(asset_id) ON DELETE CASCADE,
  to_asset_id TEXT NOT NULL REFERENCES asset(asset_id) ON DELETE CASCADE,
  relationship_type TEXT NOT NULL,
  effective_at TIMESTAMPTZ,
  metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(from_asset_id, to_asset_id, relationship_type)
);

CREATE INDEX IF NOT EXISTS ix_asset_relationship_from ON asset_relationship(from_asset_id);
CREATE INDEX IF NOT EXISTS ix_asset_relationship_to ON asset_relationship(to_asset_id);

CREATE TABLE IF NOT EXISTS asset_market_source (
  id BIGSERIAL PRIMARY KEY,
  asset_id TEXT NOT NULL REFERENCES asset(asset_id) ON DELETE CASCADE,
  source TEXT NOT NULL,
  source_asset_id TEXT NOT NULL,
  quote_asset TEXT NOT NULL,
  base_asset_symbol TEXT,
  source_market_id TEXT NOT NULL,
  valid_from TIMESTAMPTZ NOT NULL,
  valid_to TIMESTAMPTZ,
  bootstrap_from TIMESTAMPTZ NOT NULL,
  enabled BOOLEAN NOT NULL DEFAULT true,
  metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(source, source_market_id)
);

CREATE INDEX IF NOT EXISTS ix_asset_market_source_asset ON asset_market_source(asset_id);
CREATE INDEX IF NOT EXISTS ix_asset_market_source_source ON asset_market_source(source);
CREATE INDEX IF NOT EXISTS ix_asset_market_source_source_asset ON asset_market_source(source_asset_id);
CREATE INDEX IF NOT EXISTS ix_asset_market_source_validity ON asset_market_source(valid_from, valid_to);

CREATE TABLE IF NOT EXISTS asset_ohlcv (
  id BIGSERIAL PRIMARY KEY,
  asset_id TEXT NOT NULL REFERENCES asset(asset_id) ON DELETE CASCADE,
  market_source_id BIGINT NOT NULL REFERENCES asset_market_source(id) ON DELETE CASCADE,
  ts TIMESTAMPTZ NOT NULL,
  interval TEXT NOT NULL,
  open NUMERIC(38,18) NOT NULL,
  high NUMERIC(38,18) NOT NULL,
  low NUMERIC(38,18) NOT NULL,
  close NUMERIC(38,18) NOT NULL,
  volume NUMERIC(38,18) NOT NULL,
  source TEXT NOT NULL,
  source_asset_id TEXT NOT NULL,
  quote_asset TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(market_source_id, ts, interval)
);

CREATE INDEX IF NOT EXISTS ix_asset_ohlcv_asset_ts ON asset_ohlcv(asset_id, ts);
CREATE INDEX IF NOT EXISTS ix_asset_ohlcv_market_ts ON asset_ohlcv(market_source_id, ts);
CREATE INDEX IF NOT EXISTS ix_asset_ohlcv_source_symbol_ts ON asset_ohlcv(source, source_asset_id, ts);
CREATE INDEX IF NOT EXISTS ix_asset_ohlcv_ts_interval ON asset_ohlcv(ts, interval);

CREATE TABLE IF NOT EXISTS asset_indicator (
  id BIGSERIAL PRIMARY KEY,
  asset_id TEXT NOT NULL REFERENCES asset(asset_id) ON DELETE CASCADE,
  ts TIMESTAMPTZ NOT NULL,
  interval TEXT NOT NULL,
  ohlcv_source TEXT NOT NULL,
  indicator TEXT NOT NULL,
  period INTEGER NOT NULL,
  value NUMERIC(38,18) NOT NULL,
  params JSONB NOT NULL DEFAULT '{}'::jsonb,
  source TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  UNIQUE(asset_id, ts, interval, ohlcv_source, indicator, period, params, source)
);

CREATE INDEX IF NOT EXISTS ix_asset_indicator_asset_ts ON asset_indicator(asset_id, ts);
CREATE INDEX IF NOT EXISTS ix_asset_indicator_indicator ON asset_indicator(indicator);


CREATE TABLE IF NOT EXISTS etl_checkpoint (
  source TEXT NOT NULL,
  entity_id TEXT NOT NULL,
  checkpoint_key TEXT NOT NULL,
  checkpoint_value TEXT NOT NULL,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY(source, entity_id, checkpoint_key)
);

DROP TABLE IF EXISTS offchain_governance_metadata_fetch_log;
DROP TABLE IF EXISTS offchain_governance_metadata;
DROP TABLE IF EXISTS offchain_governance_source;
DROP TABLE IF EXISTS offchain_governance_fetch_log;
DROP TABLE IF EXISTS offchain_governance_proposal;

CREATE TABLE IF NOT EXISTS offchain_governance_proposal (
  id BIGSERIAL PRIMARY KEY,

  provider TEXT NOT NULL,

  source_system TEXT NOT NULL,
  source_endpoint TEXT NOT NULL,
  source_external_id TEXT NOT NULL,
  source_url TEXT NOT NULL,

  title TEXT,
  abstract TEXT,
  motivation TEXT,
  rationale TEXT,

  proposer_name TEXT,
  proposer_url TEXT,
  proposer_id TEXT,

  lifecycle_status TEXT,
  governance_action_type TEXT,

  governance_action_tx_id TEXT,
  governance_action_index INTEGER,
  governance_action_id TEXT,

  metadata_url TEXT,
  metadata_hash TEXT,

  requested_lovelace NUMERIC(38,0),

  raw_content TEXT NOT NULL,
  raw_text TEXT NOT NULL,
  content_sha256 TEXT NOT NULL,

  http_status INTEGER,
  content_type TEXT,

  first_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  fetched_at TIMESTAMPTZ NOT NULL DEFAULT now(),

  enabled BOOLEAN NOT NULL DEFAULT true,

  UNIQUE(provider, source_system, source_endpoint, source_external_id, content_sha256)
);

CREATE INDEX IF NOT EXISTS idx_offchain_gov_proposal_provider_status
  ON offchain_governance_proposal(provider, lifecycle_status);

CREATE INDEX IF NOT EXISTS idx_offchain_gov_proposal_action
  ON offchain_governance_proposal(governance_action_tx_id, governance_action_index);

CREATE INDEX IF NOT EXISTS idx_offchain_gov_proposal_action_id
  ON offchain_governance_proposal(governance_action_id);

CREATE INDEX IF NOT EXISTS idx_offchain_gov_proposal_proposer_lower
  ON offchain_governance_proposal(lower(coalesce(proposer_name, '')));

CREATE INDEX IF NOT EXISTS idx_offchain_gov_proposal_title_search
  ON offchain_governance_proposal
  USING gin(
    to_tsvector(
      'simple',
      coalesce(title, '') || ' ' ||
      coalesce(abstract, '') || ' ' ||
      coalesce(motivation, '') || ' ' ||
      coalesce(rationale, '') || ' ' ||
      coalesce(proposer_name, '')
    )
  );

CREATE TABLE IF NOT EXISTS offchain_governance_fetch_log (
  id BIGSERIAL PRIMARY KEY,
  provider TEXT NOT NULL,
  source_system TEXT NOT NULL,
  url TEXT NOT NULL,
  http_status INTEGER,
  error TEXT,
  attempted_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
