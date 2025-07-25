/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <unordered_set>

#include "velox/exec/fuzzer/AggregationFuzzerOptions.h"
#include "velox/exec/fuzzer/AggregationFuzzerRunner.h"
#include "velox/exec/fuzzer/PrestoQueryRunner.h"
#include "velox/exec/fuzzer/TransformResultVerifier.h"
#include "velox/functions/prestosql/aggregates/RegisterAggregateFunctions.h"
#include "velox/functions/prestosql/fuzzer/ApproxDistinctInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/ApproxDistinctResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/ApproxPercentileInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/ApproxPercentileResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/ArbitraryResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/AverageResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/MapUnionSumInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/MinMaxByResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/MinMaxInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/NoisyAvgInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/NoisyAvgResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/NoisyCountIfInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/NoisyCountIfResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/NoisyCountInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/NoisyCountResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/NoisySumInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/NoisySumResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/QDigestAggInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/QDigestAggResultVerifier.h"
#include "velox/functions/prestosql/fuzzer/TDigestAggregateInputGenerator.h"
#include "velox/functions/prestosql/fuzzer/TDigestAggregateResultVerifier.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"
#include "velox/functions/prestosql/window/WindowFunctionsRegistration.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

DEFINE_int64(
    seed,
    0,
    "Initial seed for random number generator used to reproduce previous "
    "results (0 means start with random seed).");

DEFINE_string(
    only,
    "",
    "If specified, Fuzzer will only choose functions from "
    "this comma separated list of function names "
    "(e.g: --only \"min\" or --only \"sum,avg\").");

DEFINE_string(
    presto_url,
    "",
    "Presto coordinator URI along with port. If set, we use Presto "
    "source of truth. Otherwise, use DuckDB. Example: "
    "--presto_url=http://127.0.0.1:8080");

DEFINE_uint32(
    req_timeout_ms,
    1000,
    "Timeout in milliseconds for HTTP requests made to reference DB, "
    "such as Presto. Example: --req_timeout_ms=2000");

// Any change made in the file should be reflected in
// the FB-internal aggregation fuzzer test too.
namespace facebook::velox::exec::test {
namespace {

std::unordered_map<std::string, std::shared_ptr<InputGenerator>>
getCustomInputGenerators() {
  return {
      {"min", std::make_shared<MinMaxInputGenerator>("min")},
      {"min_by", std::make_shared<MinMaxInputGenerator>("min_by")},
      {"max", std::make_shared<MinMaxInputGenerator>("max")},
      {"max_by", std::make_shared<MinMaxInputGenerator>("max_by")},
      {"approx_distinct", std::make_shared<ApproxDistinctInputGenerator>()},
      {"approx_set", std::make_shared<ApproxDistinctInputGenerator>()},
      {"approx_percentile", std::make_shared<ApproxPercentileInputGenerator>()},
      {"tdigest_agg", std::make_shared<TDigestAggregateInputGenerator>()},
      {"qdigest_agg", std::make_shared<QDigestAggInputGenerator>()},
      {"map_union_sum", std::make_shared<MapUnionSumInputGenerator>()},
      {"noisy_avg_gaussian", std::make_shared<NoisyAvgInputGenerator>()},
      {"noisy_count_if_gaussian",
       std::make_shared<NoisyCountIfInputGenerator>()},
      {"noisy_count_gaussian", std::make_shared<NoisyCountInputGenerator>()},
      {"noisy_sum_gaussian", std::make_shared<NoisySumInputGenerator>()},
  };
}

} // namespace
} // namespace facebook::velox::exec::test

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  // Calls common init functions in the necessary order, initializing
  // singletons, installing proper signal handlers for better debugging
  // experience, and initialize glog and gflags.
  folly::Init init(&argc, &argv);

  // Register only presto supported signatures if we are verifying against
  // Presto.
  if (FLAGS_presto_url.empty()) {
    facebook::velox::aggregate::prestosql::registerAllAggregateFunctions(
        "", false);
  } else {
    facebook::velox::aggregate::prestosql::registerAllAggregateFunctions(
        "", false, true);
  }

  facebook::velox::functions::prestosql::registerAllScalarFunctions();
  facebook::velox::window::prestosql::registerAllWindowFunctions();
  facebook::velox::functions::prestosql::registerInternalFunctions();
  facebook::velox::aggregate::prestosql::registerInternalAggregateFunctions();
  facebook::velox::memory::MemoryManager::initialize(
      facebook::velox::memory::MemoryManager::Options{});

  size_t initialSeed = FLAGS_seed == 0 ? std::time(nullptr) : FLAGS_seed;

  // List of functions that have known bugs that cause crashes or failures.
  static const std::unordered_set<std::string> skipFunctions = {
      // https://github.com/prestodb/presto/issues/24936
      "classification_fall_out",
      "classification_precision",
      "classification_recall",
      "classification_miss_rate",
      "classification_thresholds",
      // Skip internal functions used only for result verifications.
      "$internal$count_distinct",
      "$internal$array_agg",
      // https://github.com/facebookincubator/velox/issues/3493
      "stddev_pop",
      // Lambda functions are not supported yet.
      "reduce_agg",
      "max_data_size_for_stats",
      "any_value",
      // Skip non-deterministic functions.
      "noisy_approx_set_sfm",
      "noisy_approx_distinct_sfm",
      // https://github.com/facebookincubator/velox/issues/13547
      "merge",
  };

  static const std::unordered_set<std::string> functionsRequireSortedInput = {
      "tdigest_agg",
      "qdigest_agg",
  };

  using facebook::velox::exec::test::ApproxDistinctResultVerifier;
  using facebook::velox::exec::test::ApproxPercentileResultVerifier;
  using facebook::velox::exec::test::ArbitraryResultVerifier;
  using facebook::velox::exec::test::AverageResultVerifier;
  using facebook::velox::exec::test::MinMaxByResultVerifier;
  using facebook::velox::exec::test::NoisyAvgResultVerifier;
  using facebook::velox::exec::test::NoisyCountIfResultVerifier;
  using facebook::velox::exec::test::NoisyCountResultVerifier;
  using facebook::velox::exec::test::NoisySumResultVerifier;
  using facebook::velox::exec::test::QDigestAggResultVerifier;
  using facebook::velox::exec::test::setupReferenceQueryRunner;
  using facebook::velox::exec::test::TDigestAggregateResultVerifier;
  using facebook::velox::exec::test::TransformResultVerifier;

  auto makeArrayVerifier = []() {
    return TransformResultVerifier::create("\"$internal$canonicalize\"({})");
  };

  auto makeMapVerifier = []() {
    return TransformResultVerifier::create(
        "\"$internal$canonicalize\"(map_keys({}))");
  };

  // Functions whose results verification should be skipped. These can be
  // order-dependent functions whose results depend on the order of input rows,
  // or functions that return complex-typed results containing floating-point
  // fields. For some functions, the result can be transformed to a value that
  // can be verified. If such transformation exists, it can be specified to be
  // used for results verification. If no transformation is specified, results
  // are not verified.
  static const std::unordered_map<
      std::string,
      std::shared_ptr<facebook::velox::exec::test::ResultVerifier>>
      customVerificationFunctions = {
          // Order-dependent functions.
          {"approx_distinct", std::make_shared<ApproxDistinctResultVerifier>()},
          {"approx_set", std::make_shared<ApproxDistinctResultVerifier>(true)},
          {"approx_percentile",
           std::make_shared<ApproxPercentileResultVerifier>()},
          {"tdigest_agg", std::make_shared<TDigestAggregateResultVerifier>()},
          {"qdigest_agg", std::make_shared<QDigestAggResultVerifier>()},
          {"arbitrary", std::make_shared<ArbitraryResultVerifier>()},
          {"any_value", nullptr},
          {"array_agg", makeArrayVerifier()},
          {"set_agg", makeArrayVerifier()},
          {"set_union", makeArrayVerifier()},
          {"map_agg", makeMapVerifier()},
          {"map_union", makeMapVerifier()},
          {"map_union_sum", makeMapVerifier()},
          {"max_by", std::make_shared<MinMaxByResultVerifier>(false)},
          {"min_by", std::make_shared<MinMaxByResultVerifier>(true)},
          {"avg", std::make_shared<AverageResultVerifier>()},
          {"multimap_agg",
           TransformResultVerifier::create(
               "transform_values({}, (k, v) -> \"$internal$canonicalize\"(v))")},
          // Semantically inconsistent functions
          {"skewness", nullptr},
          {"kurtosis", nullptr},
          {"entropy", nullptr},
          // https://github.com/facebookincubator/velox/issues/6330
          {"max_data_size_for_stats", nullptr},
          {"sum_data_size_for_stats", nullptr},
          {"noisy_avg_gaussian", std::make_shared<NoisyAvgResultVerifier>()},
          {"noisy_count_if_gaussian",
           std::make_shared<NoisyCountIfResultVerifier>()},
          {"noisy_count_gaussian",
           std::make_shared<NoisyCountResultVerifier>()},
          {"noisy_sum_gaussian", std::make_shared<NoisySumResultVerifier>()},
      };

  using Runner = facebook::velox::exec::test::AggregationFuzzerRunner;
  using Options = facebook::velox::exec::test::AggregationFuzzerOptions;

  Options options;
  options.onlyFunctions = FLAGS_only;
  options.skipFunctions = skipFunctions;
  options.functionsRequireSortedInput = functionsRequireSortedInput;
  options.customVerificationFunctions = customVerificationFunctions;
  options.customInputGenerators =
      facebook::velox::exec::test::getCustomInputGenerators();
  options.timestampPrecision =
      facebook::velox::VectorFuzzer::Options::TimestampPrecision::kMilliSeconds;
  std::shared_ptr<facebook::velox::memory::MemoryPool> rootPool{
      facebook::velox::memory::memoryManager()->addRootPool()};

  return Runner::run(
      initialSeed,
      setupReferenceQueryRunner(
          rootPool.get(),
          FLAGS_presto_url,
          "aggregation_fuzzer",
          FLAGS_req_timeout_ms),
      options);
}
