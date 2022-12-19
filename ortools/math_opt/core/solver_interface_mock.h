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

// This header defines tools for mocking a SolverInterface.
//
// The SolverInterfaceMock mocks the SolverInterface itself. But this is usually
// not enough since we have one SolverInterface per ModelProto we solve (with
// potential updates to the initial model as ModelUpdateProto).
//
// The SolverInterfaceFactoryMock can be used to mock a solver factory function,
// along with DelegatingSolver to point to an existing SolverInterfaceMock.
//
// The SolverFactoryRegistration can be used to register the mock solver
// factory.
//
// Example of testing Solve() using all the components:
//   // Create and register the mock factory.
//   SolverInterfaceFactoryMock factory_mock;
//   SolverFactoryRegistration registration(factory_mock.AsStdFunction());
//
//   Model model;
//
//   // The mock solver that will be returned by the mock factory.
//   SolverInterfaceMock solver;
//
//   {
//     // We want to assert the sequence of calls between the factory and the
//     // solver.
//     InSequence s;
//
//     // Prepare the factory call for the next Solve() and make it return the
//     // mock solver.
//     const ModelProto expected_model = ...;
//     EXPECT_CALL(factory_mock, Call(EquivToProto(expected_model), _))
//       .WillOnce(Return(ByMove(std::make_unique<DelegatingSolver>(&solver))));
//
//     // Prepare the mock solver and its call for the next Solve().
//     const SolveResultProto result = ...;
//     EXPECT_CALL(solver, Solve(_, _, _, _))
//       .WillOnce(Return(result));
//   }
//
//   // Make the call to Solve() that is expected to make above calls using the
//   // registered SolverTypeProto.
//   ASSERT_OK_AND_ASSIGN(const SolveResult result,
//                        Solve(model, registration.solver_type(), params));
//
//
// To simulate a error in the instantiation of the solver, don't forget to use
// ByMove() (for the same reason as above, absl::StatusOr<std::unique_ptr> is
// not copyable):
//
//   EXPECT_CALL(factory_mock,
//               Call(EquivToProto(basic_lp.model->ExportModel()), _))
//       .WillOnce(Return(ByMove(absl::InternalError("oops"))));
//
#ifndef OR_TOOLS_MATH_OPT_CORE_SOLVER_INTERFACE_MOCK_H_
#define OR_TOOLS_MATH_OPT_CORE_SOLVER_INTERFACE_MOCK_H_

#include <functional>
#include <memory>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "gmock/gmock.h"
#include "ortools/base/logging.h"
#include "ortools/math_opt/callback.pb.h"
#include "ortools/math_opt/core/solve_interrupter.h"
#include "ortools/math_opt/core/solver_interface.h"
#include "ortools/math_opt/model.pb.h"
#include "ortools/math_opt/model_parameters.pb.h"
#include "ortools/math_opt/model_update.pb.h"
#include "ortools/math_opt/parameters.pb.h"
#include "ortools/math_opt/result.pb.h"

namespace operations_research {
namespace math_opt {

// A mocking of SolverInterface.
class SolverInterfaceMock : public SolverInterface {
 public:
  MOCK_METHOD(absl::StatusOr<SolveResultProto>, Solve,
              (const SolveParametersProto& parameters,
               const ModelSolveParametersProto& model_parameters,
               MessageCallback message_cb,
               const CallbackRegistrationProto& callback_registration,
               Callback cb, SolveInterrupter* interrupter),
              (override));

  MOCK_METHOD(absl::StatusOr<bool>, Update,
              (const ModelUpdateProto& model_update), (override));
};

// An implementation of SolverInterface that delegate calls to another solver
// interface.
//
// Typically used with SolverInterfaceFactoryMock to return an existing
// SolverInterfaceMock. See the comment at the top of the file for an example.
class DelegatingSolver : public SolverInterface {
 public:
  // Wraps the input solver interface, delegating calls to it. The optional
  // destructor_cb callback will be called in ~DelegatingSolver().
  explicit DelegatingSolver(SolverInterface* const solver,
                            std::function<void()> destructor_cb = nullptr)
      : solver_(ABSL_DIE_IF_NULL(solver)),
        destructor_cb_(std::move(destructor_cb)) {}

  ~DelegatingSolver() override {
    if (destructor_cb_ != nullptr) {
      destructor_cb_();
    }
  }

  absl::StatusOr<SolveResultProto> Solve(
      const SolveParametersProto& parameters,
      const ModelSolveParametersProto& model_parameters,
      MessageCallback message_cb,
      const CallbackRegistrationProto& callback_registration, Callback cb,
      SolveInterrupter* interrupter) override {
    return solver_->Solve(parameters, model_parameters, std::move(message_cb),
                          callback_registration, std::move(cb), interrupter);
  };

  absl::StatusOr<bool> Update(const ModelUpdateProto& model_update) override {
    return solver_->Update(model_update);
  };

 private:
  SolverInterface* const solver_ = nullptr;
  const std::function<void()> destructor_cb_;
};

// A mocking of a factory of solver interface.
//
// Typially registered with SolverFactoryRegistration. See the comment at the
// top of the file for an example.
using SolverInterfaceFactoryMock =
    testing::MockFunction<absl::StatusOr<std::unique_ptr<SolverInterface>>(
        const ModelProto& model, const SolverInterface::InitArgs& init_args)>;

// Creates a temporary solver interface factory registration with a fake
// SolverTypeProto.
//
// It stops calling the input factory and fail (with CHECK) when the
// registration when destroyed. The SolverTypeProto is guaranteed to be unique.
//
// This class is thread-safe.
class SolverFactoryRegistration {
 public:
  // Registers the input factory.
  //
  // Asserts (with CHECK) that the input factory is not nullptr.
  explicit SolverFactoryRegistration(SolverInterface::Factory factory);

  // Stops calling the input factory and fail instead.
  ~SolverFactoryRegistration();

  // Return the unique fake SolverTypeProto which has been used to register the
  // factory.
  SolverTypeProto solver_type() const { return solver_type_; }

 private:
  // The data shared between this class and the caller of the factory.
  struct CallerData {
    explicit CallerData(SolverInterface::Factory factory);

    absl::Mutex mutex;

    // This will be set to nullptr at the destruction of SolverFactory.
    SolverInterface::Factory factory ABSL_GUARDED_BY(mutex);
  };

  // A shared pointer on caller data shared between this class and the
  // lambda registered for solver_type_.
  std::shared_ptr<CallerData> caller_data_;

  const SolverTypeProto solver_type_;
};

}  // namespace math_opt
}  // namespace operations_research

#endif  // OR_TOOLS_MATH_OPT_CORE_SOLVER_INTERFACE_MOCK_H_
