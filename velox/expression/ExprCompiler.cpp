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

#include "velox/expression/ExprCompiler.h"
#include "velox/expression/ConstantExpr.h"
#include "velox/expression/Expr.h"
#include "velox/expression/FieldReference.h"
#include "velox/expression/LambdaExpr.h"
#include "velox/expression/RowConstructor.h"
#include "velox/expression/SimpleFunctionRegistry.h"
#include "velox/expression/SpecialFormRegistry.h"
#include "velox/expression/VectorFunction.h"

namespace facebook::velox::exec {

namespace {

using core::ITypedExpr;
using core::TypedExprPtr;

const char* const kAnd = "and";
const char* const kOr = "or";

struct ITypedExprHasher {
  size_t operator()(const ITypedExpr* expr) const {
    return expr->hash();
  }
};

struct ITypedExprComparer {
  bool operator()(const ITypedExpr* lhs, const ITypedExpr* rhs) const {
    return *lhs == *rhs;
  }
};

// Map for deduplicating ITypedExpr trees.
using ExprDedupMap = folly::F14FastMap<
    const ITypedExpr*,
    std::shared_ptr<Expr>,
    ITypedExprHasher,
    ITypedExprComparer>;

/// Represents a lexical scope. A top level scope corresponds to a top
/// level Expr and is shared among the Exprs of the ExprSet. Each
/// lambda introduces a new Scope where the 'locals' are the formal
/// parameters of the lambda. References to variables not defined in
/// a lambda's Scope are detected and added as captures to the
/// lambda. Common subexpression elimination can only take place
/// within one Scope.
struct Scope {
  // Names of variables declared in this Scope, i.e. formal parameters of a
  // lambda. Empty for a top level Scope.
  const std::vector<std::string> locals;

  // The enclosing scope, nullptr if top level scope.
  Scope* parent{nullptr};
  ExprSet* exprSet{nullptr};

  // Field names of an enclosing scope referenced from this or an inner scope.
  std::vector<std::string> capture;
  // Corresponds 1:1 to 'capture'.
  std::vector<FieldReference*> captureReferences;
  // Corresponds 1:1 to 'capture'.
  std::vector<const ITypedExpr*> captureFieldAccesses;
  // Deduplicatable ITypedExprs. Only applies within the one scope.
  ExprDedupMap visited;

  std::vector<TypedExprPtr> rewrittenExpressions;

  Scope(std::vector<std::string>&& _locals, Scope* _parent, ExprSet* _exprSet)
      : locals(std::move(_locals)), parent(_parent), exprSet(_exprSet) {}

  void addCapture(FieldReference* reference, const ITypedExpr* fieldAccess) {
    capture.emplace_back(reference->field());
    captureReferences.emplace_back(reference);
    captureFieldAccesses.emplace_back(fieldAccess);
  }
};

// Utility method to check eligibility for flattening.
bool allInputTypesEquivalent(const TypedExprPtr& expr) {
  const auto& inputs = expr->inputs();
  for (int i = 1; i < inputs.size(); i++) {
    if (!inputs[0]->type()->equivalent(*inputs[i]->type())) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> shouldFlatten(
    const TypedExprPtr& expr,
    const std::unordered_set<std::string>& flatteningCandidates) {
  if (expr->isCallKind()) {
    const auto* call = expr->asUnchecked<core::CallTypedExpr>();
    // Currently only supports the most common case for flattening where all
    // inputs are of the same type.
    if (call->name() == kAnd || call->name() == kOr ||
        (flatteningCandidates.count(call->name()) &&
         allInputTypesEquivalent(expr))) {
      return call->name();
    }
  }
  return std::nullopt;
}

bool isCall(const TypedExprPtr& expr, const std::string& name) {
  if (expr->isCallKind()) {
    return expr->asUnchecked<core::CallTypedExpr>()->name() == name;
  }
  return false;
}

// Recursively flattens nested ANDs, ORs or eligible callable expressions into a
// vector of their inputs. Recursive flattening ceases exploring an input branch
// if it encounters either an expression different from 'flattenCall' or its
// inputs are not the same type.
// Examples:
// flattenCall: AND
// in: a AND (b AND (c AND d))
// out: [a, b, c, d]
//
// flattenCall: OR
// in: (a OR b) OR (c OR d)
// out: [a, b, c, d]
//
// flattenCall: concat
// in: (array1, concat(array2, concat(array2, intVal))
// out: [array1, array2, concat(array2, intVal)]
void flattenInput(
    const TypedExprPtr& input,
    const std::string& flattenCall,
    std::vector<TypedExprPtr>& flat) {
  if (isCall(input, flattenCall) && allInputTypesEquivalent(input)) {
    for (auto& child : input->inputs()) {
      flattenInput(child, flattenCall, flat);
    }
  } else {
    flat.emplace_back(input);
  }
}

ExprPtr getAlreadyCompiled(const ITypedExpr* expr, ExprDedupMap* visited) {
  auto iter = visited->find(expr);
  return iter == visited->end() ? nullptr : iter->second;
}

ExprPtr compileExpression(
    const TypedExprPtr& expr,
    Scope* scope,
    const core::QueryConfig& config,
    memory::MemoryPool* pool,
    const std::unordered_set<std::string>& flatteningCandidates,
    bool enableConstantFolding);

std::vector<ExprPtr> compileInputs(
    const TypedExprPtr& expr,
    Scope* scope,
    const core::QueryConfig& config,
    memory::MemoryPool* pool,
    const std::unordered_set<std::string>& flatteningCandidates,
    bool enableConstantFolding) {
  std::vector<ExprPtr> compiledInputs;
  auto flattenIf = shouldFlatten(expr, flatteningCandidates);
  for (auto& input : expr->inputs()) {
    if (input->isInputKind()) {
      VELOX_CHECK(
          expr->isFieldAccessKind(),
          "An InputReference can only occur under a FieldReference");
    } else {
      if (flattenIf.has_value()) {
        std::vector<TypedExprPtr> flat;
        flattenInput(input, flattenIf.value(), flat);
        for (auto& input_2 : flat) {
          compiledInputs.push_back(compileExpression(
              input_2,
              scope,
              config,
              pool,
              flatteningCandidates,
              enableConstantFolding));
        }
      } else {
        compiledInputs.push_back(compileExpression(
            input,
            scope,
            config,
            pool,
            flatteningCandidates,
            enableConstantFolding));
      }
    }
  }
  return compiledInputs;
}

std::vector<TypePtr> getTypes(const std::vector<ExprPtr>& exprs) {
  std::vector<TypePtr> types;
  types.reserve(exprs.size());
  for (auto& expr : exprs) {
    types.emplace_back(expr->type());
  }
  return types;
}

ExprPtr getSpecialForm(
    const core::QueryConfig& config,
    const std::string& name,
    const TypePtr& type,
    std::vector<ExprPtr>&& compiledChildren,
    bool trackCpuUsage) {
  // If we just check the output of constructSpecialForm we'll have moved
  // compiledChildren, and if the function isn't a special form we'll still need
  // compiledChildren. Splitting the check in two avoids this use after move.
  if (isFunctionCallToSpecialFormRegistered(name)) {
    return constructSpecialForm(
        name, type, std::move(compiledChildren), trackCpuUsage, config);
  }

  return nullptr;
}

void captureFieldReference(
    FieldReference* reference,
    const ITypedExpr* fieldAccess,
    Scope* const referenceScope) {
  auto& field = reference->field();
  for (auto* scope = referenceScope; scope->parent; scope = scope->parent) {
    const auto& locals = scope->locals;
    auto& capture = scope->capture;
    if (std::find(locals.begin(), locals.end(), field) != locals.end() ||
        std::find(capture.begin(), capture.end(), field) != capture.end()) {
      // Return if the field is defined or captured in this scope.
      return;
    }
    scope->addCapture(reference, fieldAccess);
  }
}

std::shared_ptr<Expr> compileLambda(
    const core::LambdaTypedExpr* lambda,
    Scope* scope,
    const core::QueryConfig& config,
    memory::MemoryPool* pool,
    const std::unordered_set<std::string>& flatteningCandidates,
    bool enableConstantFolding) {
  auto signature = lambda->signature();
  auto parameterNames = signature->names();
  Scope lambdaScope(std::move(parameterNames), scope, scope->exprSet);
  auto body = compileExpression(
      lambda->body(),
      &lambdaScope,
      config,
      pool,
      flatteningCandidates,
      enableConstantFolding);

  // The lambda depends on the captures. For a lambda caller to be
  // able to peel off encodings, the captures too must be peelable.
  std::vector<std::shared_ptr<FieldReference>> captureReferences;
  captureReferences.reserve(lambdaScope.capture.size());
  for (auto i = 0; i < lambdaScope.capture.size(); ++i) {
    auto expr = lambdaScope.captureFieldAccesses[i];
    auto reference = getAlreadyCompiled(expr, &scope->visited);
    if (!reference) {
      auto inner = lambdaScope.captureReferences[i];
      reference = std::make_shared<FieldReference>(
          inner->type(), std::vector<ExprPtr>{}, inner->field());
      scope->visited[expr] = reference;
    }
    captureReferences.emplace_back(
        std::static_pointer_cast<FieldReference>(reference));
  }

  auto functionType = std::make_shared<FunctionType>(
      std::vector<TypePtr>(signature->children()), body->type());
  return std::make_shared<LambdaExpr>(
      std::move(functionType),
      std::move(signature),
      std::move(captureReferences),
      std::move(body),
      config.exprTrackCpuUsage());
}

ExprPtr tryFoldIfConstant(const ExprPtr& expr, Scope* scope) {
  if (expr->isConstantExpr() && scope->exprSet->execCtx()) {
    try {
      auto rowType = ROW({}, {});
      auto execCtx = scope->exprSet->execCtx();
      auto row = BaseVector::create<RowVector>(rowType, 1, execCtx->pool());
      EvalCtx context(execCtx, scope->exprSet, row.get());
      VectorPtr result;
      SelectivityVector rows(1);
      expr->eval(rows, context, result);
      auto constantVector = BaseVector::wrapInConstant(1, 0, std::move(result));

      auto resultExpr = std::make_shared<ConstantExpr>(constantVector);
      if (expr->stats().defaultNullRowsSkipped ||
          std::any_of(
              expr->inputs().begin(),
              expr->inputs().end(),
              [](const ExprPtr& input) {
                return input->stats().defaultNullRowsSkipped;
              })) {
        resultExpr->setDefaultNullRowsSkipped(true);
      }
      return resultExpr;
    }
    // Constant folding has a subtle gotcha: if folding a constant expression
    // deterministically throws, we can't throw at expression compilation time
    // yet because we can't guarantee that this expression would actually need
    // to be evaluated.
    //
    // So, here, if folding an expression throws an exception, we just ignore it
    // and leave the expression as-is. If this expression is hit at execution
    // time and needs to be evaluated, it will throw and fail the query anyway.
    // If not, in case this expression is never hit at execution time (for
    // instance, if other arguments are all null in a function with default null
    // behavior), the query won't fail.
    catch (const VeloxUserError&) {
    }
  }
  return expr;
}

/// Returns a vector aligned with exprs vector where elements that correspond to
/// constant expressions are set to constant values of these expressions.
/// Elements that correspond to non-constant expressions are set to null.
std::vector<VectorPtr> getConstantInputs(const std::vector<ExprPtr>& exprs) {
  std::vector<VectorPtr> constants;
  constants.reserve(exprs.size());
  for (auto& expr : exprs) {
    if (expr->isConstant()) {
      auto* constantExpr = dynamic_cast<ConstantExpr*>(expr.get());
      constants.emplace_back(constantExpr->value());
    } else {
      constants.emplace_back(nullptr);
    }
  }
  return constants;
}

core::TypedExprPtr rewriteExpression(const core::TypedExprPtr& expr) {
  for (auto& rewrite : expressionRewrites()) {
    if (auto rewritten = rewrite(expr)) {
      return rewritten;
    }
  }
  return expr;
}

ExprPtr compileCall(
    const TypedExprPtr& expr,
    std::vector<ExprPtr> inputs,
    bool trackCpuUsage,
    const core::QueryConfig& config) {
  const auto* call = expr->asUnchecked<core::CallTypedExpr>();
  const auto& resultType = expr->type();

  const auto inputTypes = getTypes(inputs);

  if (auto specialForm = specialFormRegistry().getSpecialForm(call->name())) {
    return specialForm->constructSpecialForm(
        resultType, std::move(inputs), trackCpuUsage, config);
  }

  if (auto functionWithMetadata = getVectorFunctionWithMetadata(
          call->name(), inputTypes, getConstantInputs(inputs), config)) {
    return std::make_shared<Expr>(
        resultType,
        std::move(inputs),
        functionWithMetadata->first,
        functionWithMetadata->second,
        call->name(),
        trackCpuUsage);
  }

  if (auto simpleFunctionEntry =
          simpleFunctions().resolveFunction(call->name(), inputTypes)) {
    VELOX_USER_CHECK(
        resultType->equivalent(*simpleFunctionEntry->type().get()),
        "Found incompatible return types for '{}' ({} vs. {}) "
        "for input types ({}).",
        call->name(),
        simpleFunctionEntry->type(),
        resultType,
        folly::join(", ", inputTypes));

    auto func = simpleFunctionEntry->createFunction()->createVectorFunction(
        inputTypes, getConstantInputs(inputs), config);
    return std::make_shared<Expr>(
        resultType,
        std::move(inputs),
        std::move(func),
        simpleFunctionEntry->metadata(),
        call->name(),
        trackCpuUsage);
  }

  const auto& functionName = call->name();
  auto vectorFunctionSignatures = getVectorFunctionSignatures(functionName);
  auto simpleFunctionSignatures =
      simpleFunctions().getFunctionSignatures(functionName);
  std::vector<std::string> signatures;

  if (vectorFunctionSignatures.has_value()) {
    for (const auto& signature : vectorFunctionSignatures.value()) {
      signatures.push_back(fmt::format("({})", signature->toString()));
    }
  }

  for (const auto& signature : simpleFunctionSignatures) {
    signatures.push_back(fmt::format("({})", signature->toString()));
  }

  if (signatures.empty()) {
    VELOX_USER_FAIL(
        "Scalar function name not registered: {}, called with arguments: ({}).",
        call->name(),
        folly::join(", ", inputTypes));
  } else {
    VELOX_USER_FAIL(
        "Scalar function {} not registered with arguments: ({}). "
        "Found function registered with the following signatures:\n{}",
        call->name(),
        folly::join(", ", inputTypes),
        folly::join("\n", signatures));
  }
}

ExprPtr compileCast(
    const TypedExprPtr& expr,
    std::vector<ExprPtr> inputs,
    bool trackCpuUsage,
    const core::QueryConfig& config) {
  VELOX_CHECK_EQ(1, inputs.size());

  const auto& resultType = expr->type();

  if (FOLLY_UNLIKELY(*resultType == *inputs[0]->type())) {
    return inputs[0];
  }

  const auto* cast = expr->asUnchecked<core::CastTypedExpr>();
  return getSpecialForm(
      config,
      cast->isTryCast() ? "try_cast" : "cast",
      resultType,
      std::move(inputs),
      trackCpuUsage);
}

ExprPtr compileRewrittenExpression(
    const TypedExprPtr& expr,
    Scope* scope,
    const core::QueryConfig& config,
    memory::MemoryPool* pool,
    const std::unordered_set<std::string>& flatteningCandidates,
    bool enableConstantFolding) {
  ExprPtr alreadyCompiled = getAlreadyCompiled(expr.get(), &scope->visited);
  if (alreadyCompiled) {
    if (!alreadyCompiled->isMultiplyReferenced()) {
      scope->exprSet->addToReset(alreadyCompiled);
      alreadyCompiled->setMultiplyReferenced();
      // A property of this expression changed, namely isMultiplyReferenced_,
      // that affects metadata, so we re-compute it.
      alreadyCompiled->clearMetaData();
      alreadyCompiled->computeMetadata();
    }
    return alreadyCompiled;
  }

  const bool trackCpuUsage = config.exprTrackCpuUsage();

  const auto& resultType = expr->type();
  auto compiledInputs = compileInputs(
      expr, scope, config, pool, flatteningCandidates, enableConstantFolding);

  ExprPtr result;
  switch (expr->kind()) {
    case core::ExprKind::kConcat: {
      result = getSpecialForm(
          config,
          RowConstructorCallToSpecialForm::kRowConstructor,
          resultType,
          std::move(compiledInputs),
          trackCpuUsage);
      break;
    }
    case core::ExprKind::kCast: {
      result = compileCast(expr, compiledInputs, trackCpuUsage, config);
      break;
    }
    case core::ExprKind::kCall: {
      result = compileCall(expr, compiledInputs, trackCpuUsage, config);
      break;
    }
    case core::ExprKind::kFieldAccess: {
      const auto* access = expr->asUnchecked<core::FieldAccessTypedExpr>();
      auto fieldReference = std::make_shared<FieldReference>(
          expr->type(), std::move(compiledInputs), access->name());
      if (access->isInputColumn()) {
        // We only want to capture references to top level fields, not struct
        // fields.
        captureFieldReference(fieldReference.get(), expr.get(), scope);
      }
      result = fieldReference;
      break;
    }
    case core::ExprKind::kDereference: {
      const auto* dereference = expr->asUnchecked<core::DereferenceTypedExpr>();
      result = std::make_shared<FieldReference>(
          expr->type(), std::move(compiledInputs), dereference->index());
      break;
    }
    case core::ExprKind::kInput: {
      VELOX_UNSUPPORTED("InputTypedExpr is not supported");
    }
    case core::ExprKind::kConstant: {
      const auto* constant = expr->asUnchecked<core::ConstantTypedExpr>();
      result = std::make_shared<ConstantExpr>(constant->toConstantVector(pool));
      break;
    }
    case core::ExprKind::kLambda: {
      result = compileLambda(
          expr->asUnchecked<core::LambdaTypedExpr>(),
          scope,
          config,
          pool,
          flatteningCandidates,
          enableConstantFolding);
      break;
    }
    default: {
      VELOX_UNSUPPORTED("Unknown typed expression");
    }
  }

  result->computeMetadata();

  ExprPtr compiled;
  // If the expression is constant folding it is redundant.
  if (enableConstantFolding && !result->isConstant()) {
    compiled = tryFoldIfConstant(result, scope);
    // Constant folding uses an uninitialized ExprSet for eval. This breaks the
    // invariant that 'memoizingExprs_' relies on, which is that the Expr
    // pointers will be alive for the lifetime of the ExprSet. Clear the
    // execution state of the ExprSet to avoid this.
    scope->exprSet->clear();
  } else {
    compiled = result;
  }

  scope->visited[expr.get()] = compiled;
  return compiled;
}

ExprPtr compileExpression(
    const TypedExprPtr& expr,
    Scope* scope,
    const core::QueryConfig& config,
    memory::MemoryPool* pool,
    const std::unordered_set<std::string>& flatteningCandidates,
    bool enableConstantFolding) {
  auto rewritten = rewriteExpression(expr);
  if (rewritten.get() != expr.get()) {
    scope->rewrittenExpressions.push_back(rewritten);
  }
  return compileRewrittenExpression(
      rewritten == nullptr ? expr : rewritten,
      scope,
      config,
      pool,
      flatteningCandidates,
      enableConstantFolding);
}

/// Walk expression tree and collect names of functions used in CallTypedExpr
/// into provided 'names' set.
void collectCallNames(
    const TypedExprPtr& expr,
    std::unordered_set<std::string>& names) {
  if (expr->isCallKind()) {
    names.insert(expr->asUnchecked<core::CallTypedExpr>()->name());
  }

  for (const auto& input : expr->inputs()) {
    collectCallNames(input, names);
  }
}

/// Walk expression trees and collection function calls that support flattening.
std::unordered_set<std::string> collectFlatteningCandidates(
    const std::vector<TypedExprPtr>& exprs) {
  std::unordered_set<std::string> names;
  for (const auto& expr : exprs) {
    collectCallNames(expr, names);
  }

  return vectorFunctionFactories().withRLock([&](auto& functionMap) {
    std::unordered_set<std::string> flatteningCandidates;
    for (const auto& name : names) {
      auto it = functionMap.find(name);
      if (it != functionMap.end()) {
        const auto& metadata = it->second.metadata;
        if (metadata.supportsFlattening) {
          flatteningCandidates.insert(name);
        }
      }
    }
    return flatteningCandidates;
  });
}
} // namespace

std::vector<std::shared_ptr<Expr>> compileExpressions(
    const std::vector<TypedExprPtr>& sources,
    core::ExecCtx* execCtx,
    ExprSet* exprSet,
    bool enableConstantFolding) {
  Scope scope({}, nullptr, exprSet);
  std::vector<std::shared_ptr<Expr>> exprs;
  exprs.reserve(sources.size());

  // Precompute a set of function calls that support flattening. This allows to
  // lock function registry once vs. locking for each function call.
  auto flatteningCandidates = collectFlatteningCandidates(sources);

  for (auto& source : sources) {
    exprs.push_back(compileExpression(
        source,
        &scope,
        execCtx->queryCtx()->queryConfig(),
        execCtx->pool(),
        flatteningCandidates,
        enableConstantFolding));
  }
  return exprs;
}

} // namespace facebook::velox::exec
