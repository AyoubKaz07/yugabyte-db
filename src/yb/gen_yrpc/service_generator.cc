// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/gen_yrpc/service_generator.h"

#include <google/protobuf/descriptor.h>

#include "yb/gen_yrpc/metric_descriptor.h"
#include "yb/gen_yrpc/model.h"

namespace yb {
namespace gen_yrpc {

namespace {

std::vector<MetricDescriptor> inbound_metrics = {
  {
    .name = "request_bytes",
    .prefix = "service_",
    .kind = "counter",
    .extra_args = "",
    .units = "yb::MetricUnit::kBytes",
    .description = "Bytes received by",
  },
  {
    .name = "response_bytes",
    .prefix = "service_",
    .kind = "counter",
    .extra_args = "",
    .units = "yb::MetricUnit::kBytes",
    .description = "Bytes sent in response to",
  },
  {
    .name = "handler_latency",
    .prefix = "",
    .kind = "histogram_with_percentiles",
    .extra_args = ",\n  60000000LU, 2",
    .units = "yb::MetricUnit::kMicroseconds",
    .description = "Microseconds spent handling",
  },
};

} // namespace

void ServiceGenerator::Header(YBPrinter printer, const google::protobuf::FileDescriptor *file) {
  printer(
    "// THIS FILE IS AUTOGENERATED FROM $path$\n"
    "\n"
    "#pragma once\n"
    "\n"
    "#include \"$path_no_extension$.pb.h\"\n"
  );

  if (HasLightweightMethod(file, rpc::RpcSides::SERVICE)) {
    printer("#include \"$path_no_extension$.messages.h\"\n");
  }

  printer(
    "\n"
    "#include <string>\n"
    "\n"
    "#include \"yb/rpc/rpc_fwd.h\"\n"
    "#include \"yb/rpc/rpc_header.pb.h\"\n"
    "#include \"yb/rpc/service_if.h\"\n"
    "\n"
    "#include \"yb/util/monotime.h\"\n"
    "\n"
    "namespace yb {\n"
    "class MetricEntity;\n"
    "} // namespace yb\n"
    "\n"
    "$open_namespace$"
    "\n"
    );

  for (int service_idx = 0; service_idx < file->service_count(); ++service_idx) {
    const auto* service = file->service(service_idx);
    ScopedSubstituter service_subs(printer, service);

    GenerateMethodIndexesEnum(printer, service);

    printer(
      "\n"
      "class $service_name$If : public ::yb::rpc::ServiceIf {\n"
      " public:\n"
      "  explicit $service_name$If(const scoped_refptr<MetricEntity>& entity);\n"
      "  virtual ~$service_name$If();\n"
      "  void Handle(::yb::rpc::InboundCallPtr call) override;\n"
      "  void FillEndpoints("
          "const ::yb::rpc::RpcServicePtr& service, ::yb::rpc::RpcEndpointMap* map) override;\n"
      "  std::string service_name() const override;\n"
      "  static std::string static_service_name();\n"
      "\n"
      );

    for (int method_idx = 0; method_idx < service->method_count(); ++method_idx) {
      const auto* method = service->method(method_idx);
      ScopedSubstituter method_subs(printer, method, rpc::RpcSides::SERVICE);

      if (IsTrivialMethod(method)) {
        printer(
          "  virtual ::yb::Result<$response$> $rpc_name$(\n"
          "      const $request$& req, ::yb::CoarseTimePoint deadline) = 0;\n"
        );
      } else {
        printer(
          "  virtual void $rpc_name$(\n"
          "      const $request$* req,\n"
          "      $response$* resp,\n"
          "      ::yb::rpc::RpcContext context) = 0;\n"
        );
      }
    }

    printer(
        "  \n"
        "  ::yb::rpc::RpcMethodMetrics GetMetric($service_method_enum$ index) {\n"
        "    return methods_[static_cast<size_t>(index)].metrics;\n"
        "  }\n"
        "\n"
        " private:\n"
        "  static const int kMethodCount = $service_method_count$;\n"
        "\n"
        "  // Pre-initialize metrics because calling METRIC_foo.Instantiate() is expensive.\n"
        "  void InitMethods(const scoped_refptr<MetricEntity>& ent);\n"
        "\n"
        "  ::yb::rpc::RpcMethodDesc methods_[kMethodCount];\n"
        "};\n"
    );
  }

  printer(
      "\n"
      "$close_namespace$"
  );
}

void ServiceGenerator::Source(YBPrinter printer, const google::protobuf::FileDescriptor *file) {
  printer(
    "// THIS FILE IS AUTOGENERATED FROM $path$\n"
    "\n"
    "#include \"$path_no_extension$.pb.h\"\n"
    "#include \"$path_no_extension$.service.h\"\n"
    "\n"
    "#include <glog/logging.h>\n"
    "\n"
    "#include \"yb/rpc/inbound_call.h\"\n"
    "#include \"yb/rpc/local_call.h\"\n"
    "#include \"yb/rpc/remote_method.h\"\n"
    "#include \"yb/rpc/rpc_context.h\"\n"
    "#include \"yb/rpc/rpc_service.h\"\n"
    "#include \"yb/rpc/service_if.h\"\n"
    "#include \"yb/util/metrics.h\"\n"
    "\n");

  GenerateMetricDefines(printer, file, inbound_metrics);

  printer("$open_namespace$\n");

  std::set<std::string> error_types;
  for (int service_idx = 0; service_idx < file->service_count(); ++service_idx) {
    const auto* service = file->service(service_idx);
    for (int method_idx = 0; method_idx < service->method_count(); ++method_idx) {
      auto* method = service->method(method_idx);
      if (IsTrivialMethod(method)) {
        Lightweight lightweight(IsLightweightMethod(method, rpc::RpcSides::SERVICE));
        auto resp = method->output_type();
        std::string type;
        for (int field_idx = 0; field_idx < resp->field_count(); ++field_idx) {
          auto* field = resp->field(field_idx);
          if (field->name() == "error") {
            type = MapFieldType(field, lightweight);
            break;
          } else if (field->name() == "status") {
            type = MapFieldType(field, lightweight);
            // We don't break here, since error has greater priority than status.
          }
        }
        if (!type.empty()) {
          error_types.insert(type);
        }
      }
    }
  }

  if (!error_types.empty()) {
    for (const auto& type : error_types) {
      printer("void SetupError(" + type + "* error, const Status& status);\n");
    }
    printer("\n");
  }


  for (int service_idx = 0; service_idx < file->service_count(); ++service_idx) {
    const auto* service = file->service(service_idx);
    ScopedSubstituter service_subs(printer, service);

    printer(
        "$service_name$If::$service_name$If(const scoped_refptr<MetricEntity>& entity) {\n"
        "  InitMethods(entity);\n"
        "}\n"
        "\n"
        "$service_name$If::~$service_name$If() {\n"
        "}\n"
        "\n"
        "void $service_name$If::FillEndpoints("
           "const ::yb::rpc::RpcServicePtr& service, ::yb::rpc::RpcEndpointMap* map) {\n");

    for (int method_idx = 0; method_idx < service->method_count(); ++method_idx) {
      auto* method = service->method(method_idx);
      ScopedSubstituter method_subs(printer, method, rpc::RpcSides::SERVICE);

      std::string idx_fmt = "static_cast<size_t>($service_method_enum$::$metric_enum_key$)";
      printer(
          "  map->emplace(methods_[" + idx_fmt +
              "].method.serialized_body(), std::make_pair(service, " + idx_fmt + "));\n"
      );
    }

    printer(
        "}\n"
        "\n"
        "void $service_name$If::Handle(::yb::rpc::InboundCallPtr call) {\n"
          "  auto index = call->method_index();\n"
        "  methods_[index].handler(std::move(call));\n"
        "}\n\n"
        "std::string $service_name$If::service_name() const {\n"
        "  return \"$full_service_name$\";\n"
        "}\n"
        "std::string $service_name$If::static_service_name() {\n"
        "  return \"$full_service_name$\";\n"
        "}\n\n"
        "void $service_name$If::InitMethods(const scoped_refptr<MetricEntity>& entity) {\n"
    );

    GenerateMethodAssignments(
        printer, service,
        "methods_[static_cast<size_t>($service_method_enum$::$metric_enum_key$)]",
        true, inbound_metrics);

    printer(
        "}\n"
        "\n"
    );
  }

  printer("$close_namespace$");
}

} // namespace gen_yrpc
} // namespace yb
