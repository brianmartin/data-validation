/* Copyright 2018 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_data_validation/anomalies/feature_statistics_validator.h"

#include <memory>
#include <string>

#include "absl/types/optional.h"
#include "tensorflow_data_validation/anomalies/schema.h"
#include "tensorflow_data_validation/anomalies/schema_anomalies.h"
#include "tensorflow_data_validation/anomalies/statistics_view.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow_metadata/proto/v0/schema.pb.h"

using tensorflow::metadata::v0::DatasetFeatureStatistics;

namespace tensorflow {
namespace data_validation {

namespace {
const int64 kDefaultEnumThreshold = 400;

template<typename T>
    absl::optional<T> ToAbslOptional(gtl::optional<T> opt) {
  if (!opt) return absl::nullopt;
  return std::move(opt).value();
}
}

FeatureStatisticsToProtoConfig GetDefaultFeatureStatisticsToProtoConfig() {
  FeatureStatisticsToProtoConfig feature_statistics_to_proto_config;
  feature_statistics_to_proto_config.set_enum_threshold(kDefaultEnumThreshold);
  return feature_statistics_to_proto_config;
}


tensorflow::Status InferSchema(const string& feature_statistics_proto_string,
                               const int max_string_domain_size,
                               string* schema_proto_string) {
  tensorflow::metadata::v0::DatasetFeatureStatistics feature_statistics;
  if (!feature_statistics.ParseFromString(feature_statistics_proto_string)) {
    return tensorflow::errors::InvalidArgument(
        "Failed to parse DatasetFeatureStatistics proto.");
  }
  FeatureStatisticsToProtoConfig feature_statistics_to_proto_config;
  feature_statistics_to_proto_config.set_enum_threshold(max_string_domain_size);
  tensorflow::metadata::v0::Schema schema;
  TF_RETURN_IF_ERROR(
      UpdateSchema(feature_statistics_to_proto_config,
                   schema, feature_statistics,
                   /* paths_to_consider= */ gtl::nullopt,
                   /* environment= */ gtl::nullopt, &schema));
  if (!schema.SerializeToString(schema_proto_string)) {
    return tensorflow::errors::Internal(
        "Could not serialize Schema output proto to string.");
  }
  return tensorflow::Status::OK();
}

tensorflow::Status ValidateFeatureStatistics(
    const tensorflow::metadata::v0::DatasetFeatureStatistics&
        feature_statistics,
    const tensorflow::metadata::v0::Schema& schema_proto,
    const gtl::optional<string>& environment,
    const gtl::optional<
        tensorflow::metadata::v0::DatasetFeatureStatistics>&
        prev_feature_statistics,
    const gtl::optional<
        tensorflow::metadata::v0::DatasetFeatureStatistics>&
        serving_feature_statistics,
    const gtl::optional<FeaturesNeeded>& features_needed,
    const ValidationConfig& validation_config,
    tensorflow::metadata::v0::Anomalies* result) {
  const absl::optional<string> maybe_environment =
      environment ? absl::optional<string>(*environment)
                  : absl::optional<string>();
  FeatureStatisticsToProtoConfig feature_statistics_to_proto_config;
  feature_statistics_to_proto_config.set_enum_threshold(kDefaultEnumThreshold);
  feature_statistics_to_proto_config.set_new_features_are_warnings(
      validation_config.new_features_are_warnings());
  const bool by_weight =
      DatasetStatsView(feature_statistics).WeightedStatisticsExist();
  if (feature_statistics.num_examples() == 0) {
    *result->mutable_baseline() = schema_proto;
    result->set_data_missing(true);
  } else {
    SchemaAnomalies schema_anomalies(schema_proto);
    std::shared_ptr<DatasetStatsView> previous =
        (prev_feature_statistics)
            ? std::make_shared<DatasetStatsView>(
                  prev_feature_statistics.value(), by_weight, maybe_environment,
                  /* previous= */ nullptr,
                  /* serving= */ nullptr)
            : nullptr;

    std::shared_ptr<DatasetStatsView> serving =
        (serving_feature_statistics) ? std::make_shared<DatasetStatsView>(
                                           serving_feature_statistics.value(),
                                           by_weight, maybe_environment,
                                           /* previous= */ nullptr,
                                           /* serving= */ nullptr)
                                     : nullptr;

    const DatasetStatsView training = DatasetStatsView(
        feature_statistics, by_weight, maybe_environment, previous, serving);
    TF_RETURN_IF_ERROR(
        schema_anomalies.FindChanges(training, ToAbslOptional(features_needed),
                                     feature_statistics_to_proto_config));
    *result = schema_anomalies.GetSchemaDiff();
  }

  return tensorflow::Status::OK();
}

tensorflow::Status ValidateFeatureStatistics(
    const string& feature_statistics_proto_string,
    const string& schema_proto_string, const string& environment,
    const string& previous_statistics_proto_string,
    const string& serving_statistics_proto_string,
    string* anomalies_proto_string) {
  tensorflow::metadata::v0::Schema schema;
  if (!schema.ParseFromString(schema_proto_string)) {
    return tensorflow::errors::InvalidArgument("Failed to parse Schema proto.");
  }

  tensorflow::metadata::v0::DatasetFeatureStatistics feature_statistics;
  if (!feature_statistics.ParseFromString(feature_statistics_proto_string)) {
    return tensorflow::errors::InvalidArgument(
        "Failed to parse DatasetFeatureStatistics proto.");
  }

  tensorflow::gtl::optional<tensorflow::metadata::v0::DatasetFeatureStatistics>
      previous_statistics = tensorflow::gtl::nullopt;
  if (!previous_statistics_proto_string.empty()) {
    tensorflow::metadata::v0::DatasetFeatureStatistics tmp_stats;
    if (!tmp_stats.ParseFromString(previous_statistics_proto_string)) {
      return tensorflow::errors::InvalidArgument(
          "Failed to parse DatasetFeatureStatistics proto.");
    }
    previous_statistics = tmp_stats;
  }

  tensorflow::gtl::optional<tensorflow::metadata::v0::DatasetFeatureStatistics>
      serving_statistics = tensorflow::gtl::nullopt;
  if (!serving_statistics_proto_string.empty()) {
    tensorflow::metadata::v0::DatasetFeatureStatistics tmp_stats;
    if (!tmp_stats.ParseFromString(serving_statistics_proto_string)) {
      return tensorflow::errors::InvalidArgument(
          "Failed to parse DatasetFeatureStatistics proto.");
    }
    serving_statistics = tmp_stats;
  }

  tensorflow::gtl::optional<string> may_be_environment =
      tensorflow::gtl::nullopt;
  if (!environment.empty()) {
    may_be_environment = environment;
  }

  tensorflow::metadata::v0::Anomalies anomalies;
  TF_RETURN_IF_ERROR(ValidateFeatureStatistics(
      feature_statistics, schema, may_be_environment, previous_statistics,
      serving_statistics, /*features_needed=*/gtl::nullopt, ValidationConfig(),
      &anomalies));

  if (!anomalies.SerializeToString(anomalies_proto_string)) {
    return tensorflow::errors::Internal(
        "Could not serialize Anomalies output proto to string.");
  }
  return tensorflow::Status::OK();
}

tensorflow::Status UpdateSchema(
    const FeatureStatisticsToProtoConfig& feature_statistics_to_proto_config,
    const tensorflow::metadata::v0::Schema& schema_to_update,
    const tensorflow::metadata::v0::DatasetFeatureStatistics&
        feature_statistics,
    const gtl::optional<std::vector<Path>>& paths_to_consider,
    const gtl::optional<string>& environment,
    tensorflow::metadata::v0::Schema* result) {
  const absl::optional<string> maybe_environment =
      environment ? absl::optional<string>(*environment) : absl::nullopt;

  const bool by_weight =
      DatasetStatsView(feature_statistics).WeightedStatisticsExist();
  Schema schema;
  TF_RETURN_IF_ERROR(schema.Init(schema_to_update));
  if (paths_to_consider) {
    TF_RETURN_IF_ERROR(schema.Update(
        DatasetStatsView(feature_statistics, by_weight, maybe_environment,
                         /* previous= */ nullptr,
                         /* serving= */ nullptr),
        feature_statistics_to_proto_config, *paths_to_consider));
  } else {
    TF_RETURN_IF_ERROR(schema.Update(
        DatasetStatsView(feature_statistics, by_weight, maybe_environment,
                         /* previous= */ nullptr,
                         /* serving= */ nullptr),
        feature_statistics_to_proto_config));
  }
  *result = schema.GetSchema();
  return tensorflow::Status::OK();
}

Status FeatureStatisticsValidator::ValidateFeatureStatistics(
    const metadata::v0::DatasetFeatureStatistics& feature_statistics,
    const metadata::v0::Schema& schema_proto,
    const gtl::optional<string>& environment,
    const gtl::optional<metadata::v0::DatasetFeatureStatistics>&
        prev_feature_statistics,
    const gtl::optional<metadata::v0::DatasetFeatureStatistics>&
        serving_feature_statistics,
    const gtl::optional<FeaturesNeeded>& features_needed,
    const ValidationConfig& validation_config,
    metadata::v0::Anomalies* result) const {
  return ::tensorflow::data_validation::ValidateFeatureStatistics(
      feature_statistics, schema_proto, environment, prev_feature_statistics,
      serving_feature_statistics, features_needed, validation_config, result);
}

Status FeatureStatisticsValidator::UpdateSchema(
    const FeatureStatisticsToProtoConfig& feature_statistics_to_proto_config,
    const metadata::v0::Schema& schema_to_update,
    const metadata::v0::DatasetFeatureStatistics& feature_statistics,
    const gtl::optional<std::vector<Path>>& paths_to_consider,
    const gtl::optional<string>& environment,
    metadata::v0::Schema* result) const {
  return ::tensorflow::data_validation::UpdateSchema(
      feature_statistics_to_proto_config, schema_to_update, feature_statistics,
      paths_to_consider, environment, result);
}

}  // namespace data_validation
}  // namespace tensorflow
