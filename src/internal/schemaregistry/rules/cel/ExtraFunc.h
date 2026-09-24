// Copyright 2023-2025 Buf Technologies, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "absl/status/status.h"
#include "google/protobuf/arena.h"
#include "runtime/function_registry.h"

namespace schemaregistry::rules::cel {

// Registers into the modern ::cel::FunctionRegistry. The implementations are still legacy
// google::api::expr::runtime::CelFunction instances, which is supported: CelFunction derives from
// ::cel::Function and CelFunctionDescriptor is an alias of ::cel::FunctionDescriptor, so they
// register directly and cel-cpp adapts their CelValue arguments at the boundary.
absl::Status RegisterExtraFuncs(::cel::FunctionRegistry &registry,
                               google::protobuf::Arena *regArena);

}  // namespace schemaregistry::rules::cel
