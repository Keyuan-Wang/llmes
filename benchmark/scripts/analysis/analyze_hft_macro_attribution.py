#!/usr/bin/env python3
"""Summarize benchmark-side attribution for hft_macro scenario calls.

Usage:
  python benchmark/scripts/analysis/analyze_hft_macro_attribution.py CSV [CSV ...]
  python benchmark/scripts/analysis/analyze_hft_macro_attribution.py CSV --out-dir DIR
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd


REQUIRED = {
	"op_type",
	"cycles",
	"elapsed_ns",
	"occupancy_set_path",
	"occupancy_l1_popcount_before",
	"price_mod8",
	"level_reuse_distance_ops",
}


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("csv", nargs="+", type=Path)
	parser.add_argument("--out-dir", type=Path)
	return parser.parse_args()


def latency_stats(values: pd.Series) -> pd.Series:
	return pd.Series({
		"samples": int(values.size),
		"mean": values.mean(),
		"p50": values.quantile(0.50),
		"p95": values.quantile(0.95),
		"p99": values.quantile(0.99),
		"p999": values.quantile(0.999),
		"max": values.max(),
	})


def summarize(df: pd.DataFrame, dimensions: list[str]) -> pd.DataFrame:
	parts = []
	for metric in ("cycles", "elapsed_ns"):
		part = (
			df.groupby(dimensions, observed=True)[metric]
			.apply(latency_stats)
			.unstack()
			.reset_index()
		)
		part.insert(len(dimensions), "metric", metric)
		parts.append(part)
	return pd.concat(parts, ignore_index=True)


def reuse_bin(value: float) -> str:
	if not np.isfinite(value):
		return "first_touch"
	if value <= 10:
		return "000001-000010"
	if value <= 100:
		return "000011-000100"
	if value <= 1_000:
		return "000101-001000"
	if value <= 10_000:
		return "001001-010000"
	return "010001+"


def main() -> int:
	args = parse_args()
	frames = []
	for path in args.csv:
		df = pd.read_csv(path)
		missing = REQUIRED.difference(df.columns)
		if missing:
			raise ValueError(f"{path}: missing attribution columns {sorted(missing)}")
		df = df.copy()
		df["campaign"] = path.parent.name
		frames.append(df)

	df = pd.concat(frames, ignore_index=True)
	new = df[df["op_type"] == "add_rest_new_level"].copy()
	if new.empty:
		raise ValueError("no add_rest_new_level samples")

	new["reuse_distance"] = new["level_reuse_distance_ops"].where(
		new["level_reuse_distance_ops"] >= 0, np.nan
	)
	new["reuse_bin"] = new["reuse_distance"].map(reuse_bin)

	set_path = summarize(new, ["campaign", "occupancy_set_path"])
	reuse = summarize(new, ["campaign", "reuse_bin"])
	layout = summarize(new, ["campaign", "price_mod8"])

	def correlate(frame: pd.DataFrame, distance_col: str) -> pd.DataFrame:
		rows = []
		for campaign, group in frame.dropna(subset=[distance_col]).groupby("campaign"):
			x = np.log2(group[distance_col].to_numpy(dtype=float) + 1.0)
			for metric in ("cycles", "elapsed_ns"):
				y = group[metric].to_numpy(dtype=float)
				rows.append({
					"campaign": campaign,
					"metric": metric,
					"samples": len(group),
					"pearson_log2_reuse": float(np.corrcoef(x, y)[0, 1]),
					"spearman_log2_reuse": float(
						pd.Series(x).corr(pd.Series(y), method="spearman")
					),
				})
		return pd.DataFrame(rows)

	correlations = correlate(new, "reuse_distance")

	# Order-pool-slot reuse distance: present only in CSVs produced after the
	# slot-attribution instrumentation was added. Guarded so older runs still work.
	has_slot = "order_slot_reuse_distance_ops" in new.columns
	if has_slot:
		new["slot_reuse_distance"] = new["order_slot_reuse_distance_ops"].where(
			new["order_slot_reuse_distance_ops"] >= 0, np.nan
		)
		new["slot_reuse_bin"] = new["slot_reuse_distance"].map(reuse_bin)
		slot_reuse = summarize(new, ["campaign", "slot_reuse_bin"])
		slot_correlations = correlate(new, "slot_reuse_distance")

	out_dir = args.out_dir or args.csv[0].parent
	out_dir.mkdir(parents=True, exist_ok=True)
	set_path.to_csv(out_dir / "attribution_set_path.csv", index=False)
	reuse.to_csv(out_dir / "attribution_reuse_distance.csv", index=False)
	layout.to_csv(out_dir / "attribution_price_mod8.csv", index=False)
	correlations.to_csv(out_dir / "attribution_correlations.csv", index=False)
	if has_slot:
		slot_reuse.to_csv(
			out_dir / "attribution_order_slot_reuse.csv", index=False)
		slot_correlations.to_csv(
			out_dir / "attribution_order_slot_correlations.csv", index=False)

	print("New-level latency by occupancy set path")
	print(set_path[set_path["metric"] == "cycles"].to_string(index=False))
	print("\nNew-level latency by PriceLevel reuse distance")
	print(reuse[reuse["metric"] == "cycles"].to_string(index=False))
	print("\nPriceLevel reuse-distance correlations")
	print(correlations.to_string(index=False))
	if has_slot:
		print("\nNew-level latency by order-pool-slot reuse distance")
		print(slot_reuse[slot_reuse["metric"] == "cycles"].to_string(index=False))
		print("\nOrder-pool-slot reuse-distance correlations")
		print(slot_correlations.to_string(index=False))
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
