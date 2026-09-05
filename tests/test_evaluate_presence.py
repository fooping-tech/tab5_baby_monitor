import unittest
from tools.evaluate_presence import evaluate


def row(timestamp=0, truth="present", prediction="present", latency="20"):
    return dict(session_id="synthetic", timestamp_ms=str(timestamp), truth=truth,
                prediction=prediction, pipeline_ms=latency)


class EvaluationTests(unittest.TestCase):
    def test_unknown_counts_as_miss(self):
        result = evaluate([row(), row(1, prediction="unknown"), row(2, truth="absent")])
        self.assertEqual(result["precision"], 0.5)
        self.assertEqual(result["recall_including_unknown_as_miss"], 0.5)
        self.assertAlmostEqual(result["unknown_rate"], 1 / 3)

    def test_undefined_metrics(self):
        result = evaluate([row(truth="absent", prediction="absent")])
        self.assertIsNone(result["precision"])
        self.assertIsNone(result["recall_including_unknown_as_miss"])

    def test_empty(self):
        with self.assertRaises(ValueError):
            evaluate([])

    def test_invalid(self):
        for sample in (row(latency="nan"), row(latency="-1"), row(-1),
                       row(truth="baby"), row(prediction="safe")):
            with self.subTest(sample=sample), self.assertRaises(ValueError):
                evaluate([sample])

    def test_duplicate_or_out_of_order(self):
        for timestamp in (0, 1):
            with self.assertRaises(ValueError):
                evaluate([row(1), row(timestamp)])

    def test_nearest_rank_percentiles(self):
        result = evaluate([row(timestamp=index, latency=str(index)) for index in range(1, 21)])
        self.assertEqual(result["pipeline_ms_p50"], 10)
        self.assertEqual(result["pipeline_ms_p95"], 19)


if __name__ == "__main__":
    unittest.main()
