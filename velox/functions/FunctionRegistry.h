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
#pragma once

#include <string>
#include <vector>

#include "velox/expression/FunctionMetadata.h"
#include "velox/expression/FunctionSignature.h"
#include "velox/type/Type.h"

namespace facebook::velox {

using FunctionSignatureMap = std::
    unordered_map<std::string, std::vector<const exec::FunctionSignature*>>;

/// Returns a mapping of all Simple and Vector functions registered in Velox
/// The mapping is function name -> list of function signatures
FunctionSignatureMap getFunctionSignatures();

/// Returns a list of function signatures for a given function name. Returns
/// empty list if function with specified name not found.
std::vector<const exec::FunctionSignature*> getFunctionSignatures(
    const std::string& functionName);

/// Returns a mapping of all Vector functions registered in Velox
/// The mapping is function name -> list of function signatures
FunctionSignatureMap getVectorFunctionSignatures();

/// Returns if a function is deterministic by fetching all registry entries for
/// the given function name and checking if all of them are deterministic.
/// Returns std::nullopt if the function is not found. Returns false if any of
/// the entries are not deterministic.
std::optional<bool> isDeterministic(const std::string& functionName);

/// Given a function name and argument types, returns
/// the return type if function exists otherwise returns nullptr
TypePtr resolveFunction(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Like 'resolveFunction', but with support for applying type conversions if no
/// signature matches 'argTypes' exactly.
///
/// @param coercions A list of optional type coercions that were applied to
/// resolve the function successfully. Contains one entry per argument. The
/// entry is null if no coercion is required for that argument. The entry is not
/// null if coercions is necessary.
///
/// Example, given functin plus(bigint, bigint) -> bigint and arguments
/// (integer, bigint), returns bigint with coercions = {bigint, null}. The first
/// argument needs to be coersed to bigint, while the second argument doesn't
/// require coercion.
///
/// TODO: Add support for coercion for complex and user-defined types,
/// signatures with generic types and variadic arguments.
TypePtr resolveFunctionWithCoercions(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes,
    std::vector<TypePtr>& coercions);

/// Given a function name and argument types, returns a pair of return
/// type and metadata if function exists. Otherwise, returns std::nullopt.
std::optional<std::pair<TypePtr, exec::VectorFunctionMetadata>>
resolveFunctionWithMetadata(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Given a function name and argument types, returns the return type if the
/// function exists or is a special form that supports type resolution (see
/// resolveCallableSpecialForm), otherwise returns nullptr.
TypePtr resolveFunctionOrCallableSpecialForm(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Given the name of a special form and argument types, returns
/// the return type if the special form exists and is supported, otherwise
/// returns nullptr.
/// Special forms are not supported by this function if:
/// 1) they cannot be invoked as a CallExpr, e.g. FieldReference.
/// or
/// 2) their return types cannot be inferred from their argument types, e.g.
///    Cast.
TypePtr resolveCallableSpecialForm(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Given name of simple function and argument types, returns
/// the return type if function exists otherwise returns nullptr
TypePtr resolveSimpleFunction(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Given name of vector function and argument types, returns
/// the return type if function exists otherwise returns nullptr
TypePtr resolveVectorFunction(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Given name of a vector function and argument types, returns a pair of return
/// type and metadata if function exists. Otherwise, returns std::nullopt.
std::optional<std::pair<TypePtr, exec::VectorFunctionMetadata>>
resolveVectorFunctionWithMetadata(
    const std::string& functionName,
    const std::vector<TypePtr>& argTypes);

/// Given name of a function, removes it from both the simple and vector
/// function registries (including all signatures).
void removeFunction(const std::string& functionName);

/// Clears the function registry.
void clearFunctionRegistry();

} // namespace facebook::velox
