import argparse
import csv
import json
import math


def evaluate(rows):
    matrix = {truth: {pred: 0 for pred in ("present", "absent", "unknown")}
              for truth in ("present", "absent")}
    latencies = []
    last_timestamp = {}
    for row in rows:
        truth, prediction = row["truth"], row["prediction"]
        if truth not in matrix or prediction not in matrix[truth]:
            raise ValueError("truth must be present/absent; prediction may also be unknown")
        session = row["session_id"].strip()
        timestamp = int(row["timestamp_ms"])
        latency = float(row["pipeline_ms"])
        if not session or timestamp < 0 or timestamp <= last_timestamp.get(session, -1):
            raise ValueError("session timestamps must be nonnegative and strictly increasing")
        if not math.isfinite(latency) or latency < 0:
            raise ValueError("pipeline_ms must be finite and nonnegative")
        last_timestamp[session] = timestamp
        matrix[truth][prediction] += 1
        latencies.append(latency)
    total = len(latencies)
    if not total:
        raise ValueError("empty evaluation data")
    latencies.sort()
    true_positive = matrix["present"]["present"]
    false_positive = matrix["absent"]["present"]
    positive_total = sum(matrix["present"].values())
    unknown = sum(matrix[truth]["unknown"] for truth in matrix)
    return {
        "frames": total,
        "sessions": len(last_timestamp),
        "confusion": matrix,
        "precision": true_positive / (true_positive + false_positive) if true_positive + false_positive else None,
        "recall_including_unknown_as_miss": true_positive / positive_total if positive_total else None,
        "unknown_rate": unknown / total,
        "accuracy_including_unknown_as_error": (true_positive + matrix["absent"]["absent"]) / total,
        "pipeline_ms_p50": latencies[math.ceil(total * 0.50) - 1],
        "pipeline_ms_p95": latencies[math.ceil(total * 0.95) - 1],
        "note": "Frame-weighted metrics, not time-weighted occupancy or medical validation.",
    }


def main():
    parser = argparse.ArgumentParser(description="Evaluate externally labelled presence logs without image upload")
    parser.add_argument("csv_path")
    args = parser.parse_args()
    try:
        with open(args.csv_path, newline="", encoding="utf-8") as source:
            result = evaluate(csv.DictReader(source))
    except (OSError, KeyError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
