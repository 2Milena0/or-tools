// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/linear_solver/proto_solver/sat_proto_solver.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "ortools/base/logging.h"
#include "ortools/glop/preprocessor.h"
#include "ortools/linear_solver/linear_solver.pb.h"
#include "ortools/linear_solver/model_validator.h"
#include "ortools/linear_solver/proto_solver/sat_solver_utils.h"
#include "ortools/lp_data/lp_data.h"
#include "ortools/lp_data/lp_types.h"
#include "ortools/port/proto_utils.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"
#include "ortools/sat/lp_utils.h"
#include "ortools/sat/model.h"
#include "ortools/sat/parameters_validation.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/util/logging.h"
#include "ortools/util/time_limit.h"

namespace operations_research {

namespace {

#if defined(PROTOBUF_INTERNAL_IMPL)
using google::protobuf::Message;
#else
using google::protobuf::Message;
#endif

// Proto-lite disables some features of protos and messages inherit from
// MessageLite directly instead of inheriting from Message (which is itself a
// specialization of MessageLite).
// See https://protobuf.dev/reference/cpp/cpp-generated/#message for details.
constexpr bool kProtoLiteSatParameters =
    !std::is_base_of<Message, sat::SatParameters>::value;

MPSolverResponseStatus ToMPSolverResponseStatus(sat::CpSolverStatus status,
                                                bool has_objective) {
  switch (status) {
    case sat::CpSolverStatus::UNKNOWN:
      return MPSOLVER_NOT_SOLVED;
    case sat::CpSolverStatus::MODEL_INVALID:
      return MPSOLVER_MODEL_INVALID;
    case sat::CpSolverStatus::FEASIBLE:
      return MPSOLVER_FEASIBLE;
    case sat::CpSolverStatus::INFEASIBLE:
      return MPSOLVER_INFEASIBLE;
    case sat::CpSolverStatus::OPTIMAL:
      return MPSOLVER_OPTIMAL;
    default: {
    }
  }
  return MPSOLVER_ABNORMAL;
}

sat::CpSolverStatus FromMPSolverResponseStatus(MPSolverResponseStatus status) {
  switch (status) {
    case MPSolverResponseStatus::MPSOLVER_OPTIMAL:
      return sat::OPTIMAL;
    case MPSolverResponseStatus::MPSOLVER_INFEASIBLE:
      return sat::INFEASIBLE;
    case MPSolverResponseStatus::MPSOLVER_MODEL_INVALID:
      return sat::MODEL_INVALID;
    default: {
    }
  }
  return sat::UNKNOWN;
}

MPSolutionResponse InfeasibleResponse(SolverLogger& logger,
                                      std::string message) {
  SOLVER_LOG(&logger, "Infeasible model detected in sat_solve_proto.\n",
             message);

  // This is needed for our benchmark scripts.
  if (logger.LoggingIsEnabled()) {
    sat::CpSolverResponse cp_response;
    cp_response.set_status(sat::CpSolverStatus::INFEASIBLE);
    SOLVER_LOG(&logger, CpSolverResponseStats(cp_response));
  }

  MPSolutionResponse response;
  response.set_status(MPSolverResponseStatus::MPSOLVER_INFEASIBLE);
  response.set_status_str(message);
  return response;
}

MPSolutionResponse ModelInvalidResponse(SolverLogger& logger,
                                        std::string message) {
  SOLVER_LOG(&logger, "Invalid model/parameters in sat_solve_proto.\n",
             message);

  // This is needed for our benchmark scripts.
  if (logger.LoggingIsEnabled()) {
    sat::CpSolverResponse cp_response;
    cp_response.set_status(sat::CpSolverStatus::MODEL_INVALID);
    SOLVER_LOG(&logger, CpSolverResponseStats(cp_response));
  }

  MPSolutionResponse response;
  response.set_status(MPSolverResponseStatus::MPSOLVER_MODEL_INVALID);
  response.set_status_str(message);
  return response;
}

}  // namespace

absl::StatusOr<MPSolutionResponse> SatSolveProto(
    MPModelRequest request, std::atomic<bool>* interrupt_solve,
    std::function<void(const std::string&)> logging_callback,
    std::function<void(const MPSolution&)> solution_callback) {
  sat::SatParameters params;
  params.set_log_search_progress(request.enable_internal_solver_output());
  // Set it now so that it can be overwritten by the solver specific parameters.
  if (request.has_solver_specific_parameters()) {
    // See EncodeSatParametersAsString() documentation.
    if (kProtoLiteSatParameters) {
      if (!params.MergeFromString(request.solver_specific_parameters())) {
        return absl::InvalidArgumentError(
            "solver_specific_parameters is not a valid binary stream of the "
            "SatParameters proto");
      }
    } else {
      if (!ProtobufTextFormatMergeFromString(
              request.solver_specific_parameters(), &params)) {
        return absl::InvalidArgumentError(
            "solver_specific_parameters is not a valid textual representation "
            "of the SatParameters proto");
      }
    }
  }
  if (request.has_solver_time_limit_seconds()) {
    params.set_max_time_in_seconds(request.solver_time_limit_seconds());
  }

  // TODO(user): We do not support all the parameters here. In particular the
  // logs before the solver is called will not be appended to the response. Fix
  // that, and remove code duplication for the logger config. One way should be
  // to not touch/configure anything if the logger is already created while
  // calling SolveCpModel() and call a common config function from here or from
  // inside Solve()?
  SolverLogger logger;
  if (logging_callback != nullptr) {
    logger.AddInfoLoggingCallback(logging_callback);
  }
  logger.EnableLogging(params.log_search_progress());
  logger.SetLogToStdOut(params.log_to_stdout());

  // Model validation and delta handling.
  MPSolutionResponse response;
  if (!ExtractValidMPModelInPlaceOrPopulateResponseStatus(&request,
                                                          &response)) {
    // Note that the ExtractValidMPModelInPlaceOrPopulateResponseStatus() can
    // also close trivial model (empty or trivially infeasible). So this is not
    // always the MODEL_INVALID status.
    //
    // The logging is only needed for our benchmark script, so we use UNKNOWN
    // here, but we could log the proper status instead.
    if (logger.LoggingIsEnabled()) {
      sat::CpSolverResponse cp_response;
      cp_response.set_status(FromMPSolverResponseStatus(response.status()));
      SOLVER_LOG(&logger, CpSolverResponseStats(cp_response));
    }
    return response;
  }

  // We start by some extra validation since our code do not accept any kind
  // of input.
  MPModelProto* const mp_model = request.mutable_model();
  if (!sat::MPModelProtoValidationBeforeConversion(params, *mp_model,
                                                   &logger)) {
    return ModelInvalidResponse(logger, "Extra CP-SAT validation failed.");
  }

  {
    const std::string error = sat::ValidateParameters(params);
    if (!error.empty()) {
      return ModelInvalidResponse(
          logger, absl::StrCat("Invalid CP-SAT parameters: ", error));
    }
  }

  // This is good to do before any presolve.
  if (!sat::MakeBoundsOfIntegerVariablesInteger(params, mp_model, &logger)) {
    return InfeasibleResponse(logger,
                              "An integer variable has an empty domain");
  }

  // Coefficients really close to zero can cause issues.
  // We remove them right away according to our parameters.
  RemoveNearZeroTerms(params, mp_model, &logger);

  // Note(user): the LP presolvers API is a bit weird and keep a reference to
  // the given GlopParameters, so we need to make sure it outlive them.
  const glop::GlopParameters glop_params;
  std::vector<std::unique_ptr<glop::Preprocessor>> for_postsolve;
  if (!params.enumerate_all_solutions() && params.mip_presolve_level() > 0) {
    const glop::ProblemStatus status =
        ApplyMipPresolveSteps(glop_params, mp_model, &for_postsolve, &logger);
    switch (status) {
      case glop::ProblemStatus::INIT:
        // Continue with the solve.
        break;
      case glop::ProblemStatus::PRIMAL_INFEASIBLE:
        return InfeasibleResponse(
            logger, "Problem proven infeasible during MIP presolve");
      case glop::ProblemStatus::INVALID_PROBLEM:
        return ModelInvalidResponse(
            logger, "Problem detected invalid during MIP presolve");
      default:
        // TODO(user): We put the INFEASIBLE_OR_UNBOUNBED case here since there
        // is no return status that exactly matches it.
        if (params.log_search_progress()) {
          // This is needed for our benchmark scripts.
          sat::CpSolverResponse cp_response;
          cp_response.set_status(sat::CpSolverStatus::UNKNOWN);
          SOLVER_LOG(&logger, "MIP presolve: problem infeasible or unbounded.");
          LOG(INFO) << CpSolverResponseStats(cp_response);
        }
        response.set_status(MPSolverResponseStatus::MPSOLVER_UNKNOWN_STATUS);
        if (status == glop::ProblemStatus::INFEASIBLE_OR_UNBOUNDED) {
          response.set_status_str(
              "Problem proven infeasible or unbounded during MIP presolve");
        }
        return response;
    }
  }

  // We need to do that before the automatic detection of integers.
  RemoveNearZeroTerms(params, mp_model, &logger);

  SOLVER_LOG(&logger, "");
  SOLVER_LOG(&logger, "Scaling to pure integer problem.");

  const int num_variables = mp_model->variable_size();
  std::vector<double> var_scaling(num_variables, 1.0);
  if (params.mip_automatically_scale_variables()) {
    var_scaling = sat::DetectImpliedIntegers(mp_model, &logger);
    if (!sat::MakeBoundsOfIntegerVariablesInteger(params, mp_model, &logger)) {
      return InfeasibleResponse(
          logger, "A detected integer variable has an empty domain");
    }
  }
  if (params.mip_var_scaling() != 1.0) {
    const double max_bound = params.mip_scale_large_domain()
                                 ? std::numeric_limits<double>::infinity()
                                 : params.mip_max_bound();
    const std::vector<double> other_scaling = sat::ScaleContinuousVariables(
        params.mip_var_scaling(), max_bound, mp_model);
    for (int i = 0; i < var_scaling.size(); ++i) {
      var_scaling[i] *= other_scaling[i];
    }
  }

  // Abort if one only want to solve pure-IP model and we don't have one.
  if (params.only_solve_ip()) {
    bool all_integer = true;
    for (const MPVariableProto& var : mp_model->variable()) {
      if (!var.is_integer()) {
        all_integer = false;
        break;
      }
    }
    if (!all_integer) {
      return ModelInvalidResponse(
          logger,
          "The model contains non-integer variables but the parameter "
          "'only_solve_ip' was set. Change this parameter if you "
          "still want to solve a more constrained version of the original MIP "
          "where non-integer variables can only take a finite set of values.");
    }
  }

  sat::CpModelProto cp_model;
  if (!ConvertMPModelProtoToCpModelProto(params, *mp_model, &cp_model,
                                         &logger)) {
    return ModelInvalidResponse(logger,
                                "Failed to convert model into CP-SAT model");
  }
  DCHECK_EQ(cp_model.variables().size(), var_scaling.size());
  DCHECK_EQ(cp_model.variables().size(), mp_model->variable().size());

  // Copy and scale the hint if there is one.
  if (request.model().has_solution_hint()) {
    auto* cp_model_hint = cp_model.mutable_solution_hint();
    const int size = request.model().solution_hint().var_index().size();
    for (int i = 0; i < size; ++i) {
      const int var = request.model().solution_hint().var_index(i);
      if (var >= var_scaling.size()) continue;

      // To handle weird hint input values, we cap any large value to +/-
      // mip_max_bound() which is also the min/max value of any variable once
      // scaled.
      double value =
          request.model().solution_hint().var_value(i) * var_scaling[var];
      if (std::abs(value) > params.mip_max_bound()) {
        value = value > 0 ? params.mip_max_bound() : -params.mip_max_bound();
      }

      cp_model_hint->add_vars(var);
      cp_model_hint->add_values(static_cast<int64_t>(std::round(value)));
    }
  }

  // We no longer need the request. Reclaim its memory.
  const int old_num_variables = mp_model->variable().size();
  const int old_num_constraints = mp_model->constraint().size();
  request.Clear();

  // Configure model.
  sat::Model sat_model;
  sat_model.Register<SolverLogger>(&logger);
  sat_model.Add(NewSatParameters(params));
  if (interrupt_solve != nullptr) {
    sat_model.GetOrCreate<TimeLimit>()->RegisterExternalBooleanAsLimit(
        interrupt_solve);
  }

  auto post_solve = [&](const sat::CpSolverResponse& cp_response) {
    MPSolution mp_solution;
    mp_solution.set_objective_value(cp_response.objective_value());
    // Postsolve the bound shift and scaling.
    glop::ProblemSolution glop_solution((glop::RowIndex(old_num_constraints)),
                                        (glop::ColIndex(old_num_variables)));
    for (int v = 0; v < glop_solution.primal_values.size(); ++v) {
      glop_solution.primal_values[glop::ColIndex(v)] =
          static_cast<double>(cp_response.solution(v)) / var_scaling[v];
    }
    for (int i = for_postsolve.size(); --i >= 0;) {
      for_postsolve[i]->RecoverSolution(&glop_solution);
    }
    for (int v = 0; v < glop_solution.primal_values.size(); ++v) {
      mp_solution.add_variable_value(
          glop_solution.primal_values[glop::ColIndex(v)]);
    }
    return mp_solution;
  };

  if (solution_callback != nullptr) {
    sat_model.Add(sat::NewFeasibleSolutionObserver(
        [&](const sat::CpSolverResponse& cp_response) {
          solution_callback(post_solve(cp_response));
        }));
  }

  // Solve.
  const sat::CpSolverResponse cp_response =
      sat::SolveCpModel(cp_model, &sat_model);

  // Convert the response.
  //
  // TODO(user): Implement the row and column status.
  response.mutable_solve_info()->set_solve_wall_time_seconds(
      cp_response.wall_time());
  response.mutable_solve_info()->set_solve_user_time_seconds(
      cp_response.user_time());
  response.set_status(
      ToMPSolverResponseStatus(cp_response.status(), cp_model.has_objective()));
  if (response.status() == MPSOLVER_FEASIBLE ||
      response.status() == MPSOLVER_OPTIMAL) {
    response.set_objective_value(cp_response.objective_value());
    response.set_best_objective_bound(cp_response.best_objective_bound());
    MPSolution post_solved_solution = post_solve(cp_response);
    *response.mutable_variable_value() =
        std::move(*post_solved_solution.mutable_variable_value());
  }

  // Copy and postsolve any additional solutions.
  //
  // TODO(user): Remove the postsolve hack of copying to a response.
  for (const sat::CpSolverSolution& additional_solution :
       cp_response.additional_solutions()) {
    if (absl::MakeConstSpan(additional_solution.values()) ==
        absl::MakeConstSpan(cp_response.solution())) {
      continue;
    }
    double obj = cp_model.floating_point_objective().offset();
    for (int i = 0; i < cp_model.floating_point_objective().vars_size(); ++i) {
      const int32_t var = cp_model.floating_point_objective().vars(i);
      const double obj_coef = cp_model.floating_point_objective().coeffs(i);
      obj += additional_solution.values(var) * obj_coef;
    }
    // If the scaling factor is unset/zero, it is assumed to be one.
    if (cp_model.objective().scaling_factor() != 0.0) {
      obj *= cp_model.objective().scaling_factor();
    }
    sat::CpSolverResponse temp;
    *temp.mutable_solution() = additional_solution.values();
    temp.set_objective_value(obj);
    *response.add_additional_solutions() = post_solve(temp);
  }
  const bool is_maximize = request.model().maximize();
  std::sort(response.mutable_additional_solutions()->pointer_begin(),
            response.mutable_additional_solutions()->pointer_end(),
            [is_maximize](const MPSolution* left, const MPSolution* right) {
              if (is_maximize) {
                return left->objective_value() > right->objective_value();
              } else {
                return left->objective_value() < right->objective_value();
              }
            });
  return response;
}

std::string EncodeSatParametersAsString(const sat::SatParameters& parameters) {
  if (kProtoLiteSatParameters) {
    // Here we use SerializeToString() instead of SerializeAsString() since the
    // later ignores errors and returns an empty string instead (which can be a
    // valid value when no fields are set).
    std::string bytes;
    CHECK(parameters.SerializeToString(&bytes));
    return bytes;
  }

  return ProtobufShortDebugString(parameters);
}

std::string SatSolverVersion() { return sat::CpSatSolverVersion(); }

}  // namespace operations_research
