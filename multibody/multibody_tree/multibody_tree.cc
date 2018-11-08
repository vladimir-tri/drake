#include "drake/multibody/multibody_tree/multibody_tree.h"

#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "drake/common/autodiff.h"
#include "drake/common/drake_assert.h"
#include "drake/common/drake_throw.h"
#include "drake/common/eigen_types.h"
#include "drake/multibody/multibody_tree/body_node_welded.h"
#include "drake/multibody/multibody_tree/quaternion_floating_mobilizer.h"
#include "drake/multibody/multibody_tree/rigid_body.h"
#include "drake/multibody/multibody_tree/spatial_inertia.h"

namespace drake {
namespace multibody {

using internal::BodyNode;
using internal::BodyNodeWelded;

// Helper macro to throw an exception within methods that should not be called
// post-finalize.
#define DRAKE_MBT_THROW_IF_FINALIZED() ThrowIfFinalized(__func__)

// Helper macro to throw an exception within methods that should not be called
// pre-finalize.
#define DRAKE_MBT_THROW_IF_NOT_FINALIZED() ThrowIfNotFinalized(__func__)

namespace internal {
template <typename T>
class JointImplementationBuilder {
 public:
  JointImplementationBuilder() = delete;
  static std::vector<Mobilizer<T>*> Build(
      Joint<T>* joint, MultibodyTree<T>* tree) {
    std::vector<Mobilizer<T>*> mobilizers;
    std::unique_ptr<JointBluePrint> blue_print =
        joint->MakeImplementationBlueprint();
    auto implementation = std::make_unique<JointImplementation>(*blue_print);
    DRAKE_DEMAND(implementation->num_mobilizers() != 0);
    for (auto& mobilizer : blue_print->mobilizers_) {
      mobilizers.push_back(mobilizer.get());
      tree->AddMobilizer(std::move(mobilizer));
    }
    // TODO(amcastro-tri): add force elements, bodies, constraints, etc.
    joint->OwnImplementation(std::move(implementation));
    return mobilizers;
  }
 private:
  typedef typename Joint<T>::BluePrint JointBluePrint;
  typedef typename Joint<T>::JointImplementation JointImplementation;
};
}  // namespace internal

template <typename T>
MultibodyTree<T>::MultibodyTree() {
  // Adds a "world" body to MultibodyTree having a NaN SpatialInertia.
  ModelInstanceIndex world_instance = AddModelInstance("WorldModelInstance");

  // `world_model_instance()` hardcodes the returned index.  Make sure it's
  // correct.
  DRAKE_DEMAND(world_instance == world_model_instance());
  world_body_ = &AddRigidBody("WorldBody", world_model_instance(),
                              SpatialInertia<double>());

  // `default_model_instance()` hardcodes the returned index.  Make sure it's
  // correct.
  ModelInstanceIndex default_instance =
      AddModelInstance("DefaultModelInstance");
  DRAKE_DEMAND(default_instance == default_model_instance());
}

template <typename T>
void MultibodyTree<T>::set_actuation_vector(
    ModelInstanceIndex model_instance,
    const Eigen::Ref<const VectorX<T>>& u_instance,
    EigenPtr<VectorX<T>> u) const {
  model_instances_.at(model_instance)->set_actuation_vector(u_instance, u);
}

template <typename T>
VectorX<T> MultibodyTree<T>::get_positions_from_array(
    ModelInstanceIndex model_instance,
    const Eigen::Ref<const VectorX<T>>& q_array) const {
  return model_instances_.at(model_instance)->get_positions_from_array(q_array);
}

template <class T>
void MultibodyTree<T>::set_positions_in_array(
    ModelInstanceIndex model_instance,
    const Eigen::Ref<const VectorX<T>>& model_q,
    EigenPtr<VectorX<T>> q_array) const {
  model_instances_.at(model_instance)->set_positions_in_array(model_q, q_array);
}

template <typename T>
VectorX<T> MultibodyTree<T>::get_velocities_from_array(
    ModelInstanceIndex model_instance,
    const Eigen::Ref<const VectorX<T>>& v_array) const {
  return model_instances_.at(model_instance)->get_velocities_from_array(
      v_array);
}

template <class T>
void MultibodyTree<T>::set_velocities_in_array(
    ModelInstanceIndex model_instance,
    const Eigen::Ref<const VectorX<T>>& model_v,
    EigenPtr<VectorX<T>> v_array) const {
  model_instances_.at(model_instance)->set_velocities_in_array(
      model_v, v_array);
}

template <typename T>
void MultibodyTree<T>::AddQuaternionFreeMobilizerToAllBodiesWithNoMobilizer() {
  DRAKE_DEMAND(!topology_is_valid());
  // Skip the world.
  for (BodyIndex body_index(1); body_index < num_bodies(); ++body_index) {
    const Body<T>& body = get_body(body_index);
    const BodyTopology& body_topology =
        get_topology().get_body(body.index());
    if (!body_topology.inboard_mobilizer.is_valid()) {
      std::unique_ptr<QuaternionFloatingMobilizer<T>> mobilizer =
          std::make_unique<QuaternionFloatingMobilizer<T>>(
              world_body().body_frame(), body.body_frame());
      mobilizer->set_model_instance(body.model_instance());
      this->AddMobilizer(std::move(mobilizer));
    }
  }
}

template <typename T>
const QuaternionFloatingMobilizer<T>&
MultibodyTree<T>::GetFreeBodyMobilizerOrThrow(
    const Body<T>& body) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  DRAKE_DEMAND(body.index() != world_index());
  const BodyTopology& body_topology = get_topology().get_body(body.index());
  const QuaternionFloatingMobilizer<T>* mobilizer =
      dynamic_cast<const QuaternionFloatingMobilizer<T>*>(
          &get_mobilizer(body_topology.inboard_mobilizer));
  if (mobilizer == nullptr) {
    throw std::logic_error(
        "Body '" + body.name() + "' is not a free floating body.");
  }
  return *mobilizer;
}

template <typename T>
void MultibodyTree<T>::FinalizeTopology() {
  // If the topology is valid it means that this MultibodyTree was already
  // finalized. Re-compilation is not allowed.
  if (topology_is_valid()) {
    throw std::logic_error(
        "Attempting to call MultibodyTree::FinalizeTopology() on a tree with"
        " an already finalized topology.");
  }

  // Before performing any setup that depends on the scalar type <T>, compile
  // all the type-T independent topological information.
  topology_.Finalize();
}

template <typename T>
void MultibodyTree<T>::FinalizeInternals() {
  if (!topology_is_valid()) {
    throw std::logic_error(
        "MultibodyTree::FinalizeTopology() must be called before "
        "MultibodyTree::FinalizeInternals().");
  }

  // Give different multiobody elements the chance to perform any finalize-time
  // setup.
  for (const auto& body : owned_bodies_) {
    body->SetTopology(topology_);
  }
  for (const auto& frame : owned_frames_) {
    frame->SetTopology(topology_);
  }
  for (const auto& mobilizer : owned_mobilizers_) {
    mobilizer->SetTopology(topology_);
  }
  for (const auto& force_element : owned_force_elements_) {
    force_element->SetTopology(topology_);
  }
  for (const auto& actuator : owned_actuators_) {
    actuator->SetTopology(topology_);
  }

  body_node_levels_.resize(topology_.tree_height());
  for (BodyNodeIndex body_node_index(1);
       body_node_index < topology_.get_num_body_nodes(); ++body_node_index) {
    const BodyNodeTopology& node_topology =
        topology_.get_body_node(body_node_index);
    body_node_levels_[node_topology.level].push_back(body_node_index);
  }

  // Creates BodyNode's:
  // This recursion order ensures that a BodyNode's parent is created before the
  // node itself, since BodyNode objects are in Breadth First Traversal order.
  for (BodyNodeIndex body_node_index(0);
       body_node_index < topology_.get_num_body_nodes(); ++body_node_index) {
    CreateBodyNode(body_node_index);
  }

  CreateModelInstances();
}

template <typename T>
void MultibodyTree<T>::Finalize() {
  DRAKE_MBT_THROW_IF_FINALIZED();
  // Create Joint objects's implementation. Joints are implemented using a
  // combination of MultibodyTree's building blocks such as Body, Mobilizer,
  // ForceElement and Constraint. For a same physical Joint, several
  // implementations could be created (for instance, a Constraint instead of a
  // Mobilizer). The decision on what implementation to create is performed by
  // MultibodyTree at Finalize() time. Then, JointImplementationBuilder below
  // can request MultibodyTree for these choices when building the Joint
  // implementation. Since a Joint's implementation is built upon
  // MultibodyTree's building blocks, notice that creating a Joint's
  // implementation will therefore change the tree topology. Since topology
  // changes are NOT allowed after Finalize(), joint implementations MUST be
  // assembled BEFORE the tree's topology is finalized.
  for (auto& joint : owned_joints_) {
    std::vector<Mobilizer<T>*> mobilizers =
        internal::JointImplementationBuilder<T>::Build(joint.get(), this);
    for (Mobilizer<T>* mobilizer : mobilizers) {
      mobilizer->set_model_instance(joint->model_instance());
    }
  }
  // It is VERY important to add quaternions if needed only AFTER joints had a
  // chance to get implemented with mobilizers. This is because joints's
  // implementations change the topology of the tree. Therefore, do not change
  // this order!
  AddQuaternionFreeMobilizerToAllBodiesWithNoMobilizer();
  FinalizeTopology();
  FinalizeInternals();
}

template <typename T>
void MultibodyTree<T>::CreateBodyNode(BodyNodeIndex body_node_index) {
  const BodyNodeTopology& node_topology =
      topology_.get_body_node(body_node_index);
  const BodyIndex body_index = node_topology.body;

  const Body<T>* body = owned_bodies_[node_topology.body].get();

  std::unique_ptr<BodyNode<T>> body_node;
  if (body_index == world_index()) {
    body_node = std::make_unique<BodyNodeWelded<T>>(&world_body());
  } else {
    // The mobilizer should be valid if not at the root (the world).
    DRAKE_ASSERT(node_topology.mobilizer.is_valid());
    const Mobilizer<T>* mobilizer =
        owned_mobilizers_[node_topology.mobilizer].get();

    BodyNode<T>* parent_node =
        body_nodes_[node_topology.parent_body_node].get();

    // Only the mobilizer knows how to create a body node with compile-time
    // fixed sizes.
    body_node = mobilizer->CreateBodyNode(parent_node, body, mobilizer);
    parent_node->add_child_node(body_node.get());
  }
  body_node->set_parent_tree(this, body_node_index);
  body_node->SetTopology(topology_);

  body_nodes_.push_back(std::move(body_node));
}

template <typename T>
void MultibodyTree<T>::CreateModelInstances() {
  DRAKE_ASSERT(model_instances_.empty());

  // First create the pool of instances.
  for (ModelInstanceIndex model_instance_index(0);
       model_instance_index < num_model_instances(); ++model_instance_index) {
    std::unique_ptr<internal::ModelInstance<T>> model_instance =
        std::make_unique<internal::ModelInstance<T>>(model_instance_index);
    model_instance->set_parent_tree(this, model_instance_index);
    model_instances_.push_back(std::move(model_instance));
  }

  // Add all of our mobilizers and joint actuators to the appropriate instance.
  // The order of the mobilizers should match the order in which the bodies were
  // added to the tree, which may not be the order in which the mobilizers were
  // added, so we get the mobilizer through the BodyNode.
  for (const auto& body_node : body_nodes_) {
    if (body_node->get_num_mobilizer_positions() > 0 ||
        body_node->get_num_mobilizer_velocities() > 0) {
      model_instances_.at(body_node->model_instance())->add_mobilizer(
          &body_node->get_mobilizer());
    }
  }

  for (const auto& joint_actuator : owned_actuators_) {
    model_instances_.at(joint_actuator->model_instance())->add_joint_actuator(
        joint_actuator.get());
  }
}

template <typename T>
void MultibodyTree<T>::SetDefaultContext(systems::Context<T> *context) const {
  for (const auto& mobilizer : owned_mobilizers_) {
    mobilizer->set_zero_configuration(context);
  }
}

template <typename T>
void MultibodyTree<T>::SetDefaultState(
    const systems::Context<T>& context, systems::State<T>* state) const {
  for (const auto& mobilizer : owned_mobilizers_) {
    mobilizer->set_zero_state(context, state);
  }
}

template <typename T>
Eigen::VectorBlock<const VectorX<T>>
MultibodyTree<T>::get_multibody_state_vector(
    const systems::Context<T>& context) const {
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);
  return mbt_context.get_state_vector();
}

template <typename T>
Eigen::VectorBlock<VectorX<T>>
MultibodyTree<T>::get_mutable_multibody_state_vector(
    systems::Context<T>* context) const {
  DRAKE_DEMAND(context != nullptr);
  auto* mbt_context = dynamic_cast<MultibodyTreeContext<T>*>(context);
  if (mbt_context == nullptr) {
    throw std::runtime_error(
        "The context provided is not compatible with a multibody model.");
  }
  return mbt_context->get_mutable_state_vector();
}

template <typename T>
void MultibodyTree<T>::SetFreeBodyPoseOrThrow(
    const Body<T>& body, const Isometry3<T>& X_WB,
    systems::Context<T>* context) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  SetFreeBodyPoseOrThrow(body, X_WB, *context, &context->get_mutable_state());
}

template <typename T>
void MultibodyTree<T>::SetFreeBodySpatialVelocityOrThrow(
    const Body<T>& body, const SpatialVelocity<T>& V_WB,
    systems::Context<T>* context) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  SetFreeBodySpatialVelocityOrThrow(
      body, V_WB, *context, &context->get_mutable_state());
}

template <typename T>
void MultibodyTree<T>::SetFreeBodyPoseOrThrow(
    const Body<T>& body, const Isometry3<T>& X_WB,
    const systems::Context<T>& context, systems::State<T>* state) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  const QuaternionFloatingMobilizer<T>& mobilizer =
      GetFreeBodyMobilizerOrThrow(body);
  mobilizer.set_quaternion(context, Quaternion<T>(X_WB.linear()), state);
  mobilizer.set_position(context, X_WB.translation(), state);
}

template <typename T>
void MultibodyTree<T>::SetFreeBodySpatialVelocityOrThrow(
    const Body<T>& body, const SpatialVelocity<T>& V_WB,
    const systems::Context<T>& context, systems::State<T>* state) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  const QuaternionFloatingMobilizer<T>& mobilizer =
      GetFreeBodyMobilizerOrThrow(body);
  mobilizer.set_angular_velocity(context, V_WB.rotational(), state);
  mobilizer.set_translational_velocity(context, V_WB.translational(), state);
}

template <typename T>
void MultibodyTree<T>::CalcAllBodyPosesInWorld(
    const systems::Context<T>& context,
    std::vector<Isometry3<T>>* X_WB) const {
  DRAKE_THROW_UNLESS(X_WB != nullptr);
  if (static_cast<int>(X_WB->size()) != num_bodies()) {
    X_WB->resize(num_bodies(), Isometry3<T>::Identity());
  }
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const BodyNodeIndex node_index = get_body(body_index).node_index();
    X_WB->at(body_index) = pc.get_X_WB(node_index);
  }
}

template <typename T>
void MultibodyTree<T>::CalcAllBodySpatialVelocitiesInWorld(
    const systems::Context<T>& context,
    std::vector<SpatialVelocity<T>>* V_WB) const {
  DRAKE_THROW_UNLESS(V_WB != nullptr);
  if (static_cast<int>(V_WB->size()) != num_bodies()) {
    V_WB->resize(num_bodies(), SpatialVelocity<T>::Zero());
  }
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);
  for (BodyIndex body_index(0); body_index < num_bodies(); ++body_index) {
    const BodyNodeIndex node_index = get_body(body_index).node_index();
    V_WB->at(body_index) = vc.get_V_WB(node_index);
  }
}

template <typename T>
void MultibodyTree<T>::CalcPositionKinematicsCache(
    const systems::Context<T>& context,
    PositionKinematicsCache<T>* pc) const {
  DRAKE_DEMAND(pc != nullptr);
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  // TODO(amcastro-tri): Loop over bodies to update their position dependent
  // kinematics. This gives the chance to flexible bodies to update the pose
  // X_BQ(qb_B) of each frame Q that is attached to the body.
  // Notice this loop can be performed in any order and each X_BQ(qf_B) is
  // independent of all others. This could even be performed in parallel.

  // With the kinematics information across mobilizer's and the kinematics
  // information for each body, we are now in position to perform a base-to-tip
  // recursion to update world positions and parent to child body transforms.
  // This skips the world, level = 0.
  for (int level = 1; level < tree_height(); ++level) {
    for (BodyNodeIndex body_node_index : body_node_levels_[level]) {
      const BodyNode<T>& node = *body_nodes_[body_node_index];

      DRAKE_ASSERT(node.get_topology().level == level);
      DRAKE_ASSERT(node.index() == body_node_index);

      // Update per-node kinematics.
      node.CalcPositionKinematicsCache_BaseToTip(mbt_context, pc);
    }
  }
}

template <typename T>
void MultibodyTree<T>::CalcVelocityKinematicsCache(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    VelocityKinematicsCache<T>* vc) const {
  DRAKE_DEMAND(vc != nullptr);
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  // TODO(amcastro-tri): Loop over bodies to compute velocity kinematics updates
  // corresponding to flexible bodies.

  // TODO(amcastro-tri): Eval H_PB_W from the cache.
  std::vector<Vector6<T>> H_PB_W_cache(num_velocities());
  CalcAcrossNodeGeometricJacobianExpressedInWorld(context, pc, &H_PB_W_cache);

  // Performs a base-to-tip recursion computing body velocities.
  // This skips the world, depth = 0.
  for (int depth = 1; depth < tree_height(); ++depth) {
    for (BodyNodeIndex body_node_index : body_node_levels_[depth]) {
      const BodyNode<T>& node = *body_nodes_[body_node_index];

      DRAKE_ASSERT(node.get_topology().level == depth);
      DRAKE_ASSERT(node.index() == body_node_index);

      // Jacobian matrix for this node. H_PB_W ∈ ℝ⁶ˣⁿᵐ with nm ∈ [0; 6] the
      // number of mobilities for this node. Therefore, the return is a
      // MatrixUpTo6 since the number of columns generally changes with the
      // node.
      // It is returned as an Eigen::Map to the memory allocated in the
      // std::vector H_PB_W_cache so that we can work with H_PB_W as with any
      // other Eigen matrix object.
      Eigen::Map<const MatrixUpTo6<T>> H_PB_W =
          node.GetJacobianFromArray(H_PB_W_cache);

      // Update per-node kinematics.
      node.CalcVelocityKinematicsCache_BaseToTip(mbt_context, pc, H_PB_W, vc);
    }
  }
}

template <typename T>
void MultibodyTree<T>::CalcSpatialAccelerationsFromVdot(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc,
    const VectorX<T>& known_vdot,
    std::vector<SpatialAcceleration<T>>* A_WB_array) const {
  DRAKE_DEMAND(A_WB_array != nullptr);
  DRAKE_DEMAND(static_cast<int>(A_WB_array->size()) == num_bodies());

  DRAKE_DEMAND(known_vdot.size() == topology_.num_velocities());

  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  // TODO(amcastro-tri): Loop over bodies to compute acceleration kinematics
  // updates corresponding to flexible bodies.

  // The world's spatial acceleration is always zero.
  A_WB_array->at(world_index()) = SpatialAcceleration<T>::Zero();

  // Performs a base-to-tip recursion computing body accelerations.
  // This skips the world, depth = 0.
  for (int depth = 1; depth < tree_height(); ++depth) {
    for (BodyNodeIndex body_node_index : body_node_levels_[depth]) {
      const BodyNode<T>& node = *body_nodes_[body_node_index];

      DRAKE_ASSERT(node.get_topology().level == depth);
      DRAKE_ASSERT(node.index() == body_node_index);

      // Update per-node kinematics.
      node.CalcSpatialAcceleration_BaseToTip(
          mbt_context, pc, vc, known_vdot, A_WB_array);
    }
  }
}

template <typename T>
void MultibodyTree<T>::CalcAccelerationKinematicsCache(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc,
    const VectorX<T>& known_vdot,
    AccelerationKinematicsCache<T>* ac) const {
  DRAKE_DEMAND(ac != nullptr);
  DRAKE_DEMAND(known_vdot.size() == topology_.num_velocities());

  // TODO(amcastro-tri): Loop over bodies to compute velocity kinematics updates
  // corresponding to flexible bodies.

  std::vector<SpatialAcceleration<T>>& A_WB_array = ac->get_mutable_A_WB_pool();

  CalcSpatialAccelerationsFromVdot(context, pc, vc, known_vdot, &A_WB_array);
}

template <typename T>
VectorX<T> MultibodyTree<T>::CalcInverseDynamics(
    const systems::Context<T>& context,
    const VectorX<T>& known_vdot,
    const MultibodyForces<T>& external_forces) const {
  // Temporary storage used in the computation of inverse dynamics.
  std::vector<SpatialAcceleration<T>> A_WB(num_bodies());
  std::vector<SpatialForce<T>> F_BMo_W(num_bodies());

  const auto& pc = EvalPositionKinematics(context);
  const auto& vc = EvalVelocityKinematics(context);
  VectorX<T> tau(num_velocities());
  CalcInverseDynamics(
      context, pc, vc, known_vdot,
      external_forces.body_forces(), external_forces.generalized_forces(),
      &A_WB, &F_BMo_W, &tau);
  return tau;
}

template <typename T>
void MultibodyTree<T>::CalcInverseDynamics(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc,
    const VectorX<T>& known_vdot,
    const std::vector<SpatialForce<T>>& Fapplied_Bo_W_array,
    const Eigen::Ref<const VectorX<T>>& tau_applied_array,
    std::vector<SpatialAcceleration<T>>* A_WB_array,
    std::vector<SpatialForce<T>>* F_BMo_W_array,
    EigenPtr<VectorX<T>> tau_array) const {
  DRAKE_DEMAND(known_vdot.size() == num_velocities());
  const int Fapplied_size = static_cast<int>(Fapplied_Bo_W_array.size());
  DRAKE_DEMAND(Fapplied_size == num_bodies() || Fapplied_size == 0);
  const int tau_applied_size = tau_applied_array.size();
  DRAKE_DEMAND(
      tau_applied_size == num_velocities() || tau_applied_size == 0);

  DRAKE_DEMAND(A_WB_array != nullptr);
  DRAKE_DEMAND(static_cast<int>(A_WB_array->size()) == num_bodies());

  DRAKE_DEMAND(F_BMo_W_array != nullptr);
  DRAKE_DEMAND(static_cast<int>(F_BMo_W_array->size()) == num_bodies());

  DRAKE_DEMAND(tau_array->size() == num_velocities());

  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  // Compute body spatial accelerations given the generalized accelerations are
  // known.
  CalcSpatialAccelerationsFromVdot(context, pc, vc, known_vdot, A_WB_array);

  // Vector of generalized forces per mobilizer.
  // It has zero size if no forces are applied.
  VectorUpTo6<T> tau_applied_mobilizer(0);

  // Spatial force applied on B at Bo.
  // It is left initialized to zero if no forces are applied.
  SpatialForce<T> Fapplied_Bo_W = SpatialForce<T>::Zero();

  // Performs a tip-to-base recursion computing the total spatial force F_BMo_W
  // acting on body B, about point Mo, expressed in the world frame W.
  // This includes the world (depth = 0) so that F_BMo_W_array[world_index()]
  // contains the total force of the bodies connected to the world by a
  // mobilizer.
  for (int depth = tree_height() - 1; depth >= 0; --depth) {
    for (BodyNodeIndex body_node_index : body_node_levels_[depth]) {
      const BodyNode<T>& node = *body_nodes_[body_node_index];

      DRAKE_ASSERT(node.get_topology().level == depth);
      DRAKE_ASSERT(node.index() == body_node_index);

      // Make a copy to the total applied forces since the call to
      // CalcInverseDynamics_TipToBase() below could overwrite the entry for the
      // current body node if the input applied forces arrays are the same
      // in-memory object as the output arrays.
      // This allows users to specify the same input and output arrays if
      // desired to minimize memory footprint.
      // Leave them initialized to zero if no applied forces were provided.
      if (tau_applied_size != 0) {
        tau_applied_mobilizer =
            node.get_mobilizer().get_generalized_forces_from_array(
                tau_applied_array);
      }
      if (Fapplied_size != 0) {
        Fapplied_Bo_W = Fapplied_Bo_W_array[body_node_index];
      }

      // Compute F_BMo_W for the body associated with this node and project it
      // onto the space of generalized forces for the associated mobilizer.
      node.CalcInverseDynamics_TipToBase(
          mbt_context, pc, vc, *A_WB_array,
          Fapplied_Bo_W, tau_applied_mobilizer,
          F_BMo_W_array, tau_array);
    }
  }
}

template <typename T>
void MultibodyTree<T>::CalcForceElementsContribution(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc,
    MultibodyForces<T>* forces) const {
  DRAKE_DEMAND(forces != nullptr);
  DRAKE_DEMAND(forces->CheckHasRightSizeForModel(*this));

  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  forces->SetZero();
  // Add contributions from force elements.
  for (const auto& force_element : owned_force_elements_) {
    force_element->CalcAndAddForceContribution(mbt_context, pc, vc, forces);
  }

  // TODO(amcastro-tri): Remove this call once damping is implemented in terms
  // of force elements.
  AddJointDampingForces(context, forces);
}

template<typename T>
void MultibodyTree<T>::AddJointDampingForces(
    const systems::Context<T>& context, MultibodyForces<T>* forces) const {
  DRAKE_DEMAND(forces != nullptr);
  for (const auto& joint : owned_joints_) {
    joint->AddInDamping(context, forces);
  }
}

template <typename T>
void MultibodyTree<T>::MapQDotToVelocity(
    const systems::Context<T>& context,
    const Eigen::Ref<const VectorX<T>>& qdot,
    EigenPtr<VectorX<T>> v) const {
  DRAKE_DEMAND(qdot.size() == num_positions());
  DRAKE_DEMAND(v != nullptr);
  DRAKE_DEMAND(v->size() == num_velocities());
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);
  VectorUpTo6<T> v_mobilizer;
  for (const auto& mobilizer : owned_mobilizers_) {
    const auto qdot_mobilizer = mobilizer->get_positions_from_array(qdot);
    v_mobilizer.resize(mobilizer->num_velocities());
    mobilizer->MapQDotToVelocity(mbt_context, qdot_mobilizer, &v_mobilizer);
    mobilizer->get_mutable_velocities_from_array(v) = v_mobilizer;
  }
}

template <typename T>
void MultibodyTree<T>::MapVelocityToQDot(
    const systems::Context<T>& context,
    const Eigen::Ref<const VectorX<T>>& v,
    EigenPtr<VectorX<T>> qdot) const {
  DRAKE_DEMAND(v.size() == num_velocities());
  DRAKE_DEMAND(qdot != nullptr);
  DRAKE_DEMAND(qdot->size() == num_positions());
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);
  const int kMaxQdot = 7;
  // qdot_mobilizer is a dynamic sized vector of max size equal to seven.
  Eigen::Matrix<T, Eigen::Dynamic, 1, 0, kMaxQdot, 1> qdot_mobilizer;
  for (const auto& mobilizer : owned_mobilizers_) {
    const auto v_mobilizer = mobilizer->get_velocities_from_array(v);
    DRAKE_DEMAND(mobilizer->num_positions() <= kMaxQdot);
    qdot_mobilizer.resize(mobilizer->num_positions());
    mobilizer->MapVelocityToQDot(mbt_context, v_mobilizer, &qdot_mobilizer);
    mobilizer->get_mutable_positions_from_array(qdot) = qdot_mobilizer;
  }
}

template <typename T>
void MultibodyTree<T>::CalcMassMatrixViaInverseDynamics(
    const systems::Context<T>& context, EigenPtr<MatrixX<T>> H) const {
  DRAKE_DEMAND(H != nullptr);
  DRAKE_DEMAND(H->rows() == num_velocities());
  DRAKE_DEMAND(H->cols() == num_velocities());
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  DoCalcMassMatrixViaInverseDynamics(context, pc, H);
}

template <typename T>
void MultibodyTree<T>::DoCalcMassMatrixViaInverseDynamics(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    EigenPtr<MatrixX<T>> H) const {
  // TODO(amcastro-tri): Consider passing a boolean flag to tell
  // CalcInverseDynamics() to ignore velocity dependent terms.
  VelocityKinematicsCache<T> vc(get_topology());
  vc.InitializeToZero();

  // Compute one column of the mass matrix via inverse dynamics at a time.
  const int nv = num_velocities();
  VectorX<T> vdot(nv);
  VectorX<T> tau(nv);
  // Auxiliary arrays used by inverse dynamics.
  std::vector<SpatialAcceleration<T>> A_WB_array(num_bodies());
  std::vector<SpatialForce<T>> F_BMo_W_array(num_bodies());

  for (int j = 0; j < nv; ++j) {
    vdot = VectorX<T>::Unit(nv, j);
    tau.setZero();
    CalcInverseDynamics(context, pc, vc, vdot, {}, VectorX<T>(),
                        &A_WB_array, &F_BMo_W_array, &tau);
    H->col(j) = tau;
  }
}

template <typename T>
void MultibodyTree<T>::CalcBiasTerm(
    const systems::Context<T>& context, EigenPtr<VectorX<T>> Cv) const {
  DRAKE_DEMAND(Cv != nullptr);
  DRAKE_DEMAND(Cv->rows() == num_velocities());
  DRAKE_DEMAND(Cv->cols() == 1);
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);
  DoCalcBiasTerm(context, pc, vc, Cv);
}

template <typename T>
VectorX<T> MultibodyTree<T>::CalcGravityGeneralizedForces(
    const systems::Context<T>& context) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  if (gravity_field_.has_value()) {
    return gravity_field_.value()->CalcGravityGeneralizedForces(context);
  }
  return VectorX<T>::Zero(num_velocities());
}

template <typename T>
void MultibodyTree<T>::DoCalcBiasTerm(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc,
    EigenPtr<VectorX<T>> Cv) const {
  const int nv = num_velocities();
  const VectorX<T> vdot = VectorX<T>::Zero(nv);

  // Auxiliary arrays used by inverse dynamics.
  std::vector<SpatialAcceleration<T>> A_WB_array(num_bodies());
  std::vector<SpatialForce<T>> F_BMo_W_array(num_bodies());

  // TODO(amcastro-tri): provide specific API for when vdot = 0.
  CalcInverseDynamics(context, pc, vc, vdot, {}, VectorX<T>(),
                      &A_WB_array, &F_BMo_W_array, Cv);
}

template <typename T>
Isometry3<T> MultibodyTree<T>::CalcRelativeTransform(
    const systems::Context<T>& context,
    const Frame<T>& frame_A, const Frame<T>& frame_B) const {
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const Isometry3<T>& X_WA =
      pc.get_X_WB(frame_A.body().node_index()) *
      frame_A.CalcPoseInBodyFrame(context);
  const Isometry3<T>& X_WB =
      pc.get_X_WB(frame_B.body().node_index()) *
      frame_B.CalcPoseInBodyFrame(context);
  return X_WA.inverse() * X_WB;
}

template <typename T>
void MultibodyTree<T>::CalcPointsPositions(
    const systems::Context<T>& context,
    const Frame<T>& frame_B,
    const Eigen::Ref<const MatrixX<T>>& p_BQi,
    const Frame<T>& frame_A,
    EigenPtr<MatrixX<T>> p_AQi) const {
  DRAKE_THROW_UNLESS(p_BQi.rows() == 3);
  DRAKE_THROW_UNLESS(p_AQi != nullptr);
  DRAKE_THROW_UNLESS(p_AQi->rows() == 3);
  DRAKE_THROW_UNLESS(p_AQi->cols() == p_BQi.cols());
  const Isometry3<T> X_AB =
      CalcRelativeTransform(context, frame_A, frame_B);
  // We demanded above that these matrices have three rows. Therefore we tell
  // Eigen so.
  p_AQi->template topRows<3>() = X_AB * p_BQi.template topRows<3>();
}

template <typename T>
const Isometry3<T>& MultibodyTree<T>::EvalBodyPoseInWorld(
    const systems::Context<T>& context,
    const Body<T>& body_B) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  body_B.HasThisParentTreeOrThrow(this);
  return EvalPositionKinematics(context).get_X_WB(body_B.node_index());
}

template <typename T>
const SpatialVelocity<T>& MultibodyTree<T>::EvalBodySpatialVelocityInWorld(
    const systems::Context<T>& context,
    const Body<T>& body_B) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();
  body_B.HasThisParentTreeOrThrow(this);
  return EvalVelocityKinematics(context).get_V_WB(body_B.node_index());
}

template <typename T>
void MultibodyTree<T>::CalcAcrossNodeGeometricJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    std::vector<Vector6<T>>* H_PB_W_cache) const {
  DRAKE_DEMAND(H_PB_W_cache != nullptr);
  DRAKE_DEMAND(static_cast<int>(H_PB_W_cache->size()) == num_velocities());

  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  for (BodyNodeIndex node_index(1);
       node_index < num_bodies(); ++node_index) {
    const BodyNode<T>& node = *body_nodes_[node_index];

    // Jacobian matrix for this node. H_PB_W ∈ ℝ⁶ˣⁿᵐ with nm ∈ [0; 6] the number
    // of mobilities for this node. Therefore, the return is a MatrixUpTo6 since
    // the number of columns generally changes with the node.
    // It is returned as an Eigen::Map to the memory allocated in the
    // std::vector H_PB_W_cache so that we can work with H_PB_W as with any
    // other Eigen matrix object.
    Eigen::Map<MatrixUpTo6<T>> H_PB_W =
        node.GetMutableJacobianFromArray(H_PB_W_cache);

    node.CalcAcrossNodeGeometricJacobianExpressedInWorld(
        mbt_context, pc, &H_PB_W);
  }
}

template <typename T>
void MultibodyTree<T>::CalcPointsGeometricJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const Frame<T>& frame_B, const Eigen::Ref<const MatrixX<T>>& p_BQi_set,
    EigenPtr<MatrixX<T>> p_WQi_set, EigenPtr<MatrixX<T>> Jv_WQi) const {
  DRAKE_THROW_UNLESS(p_BQi_set.rows() == 3);
  const int num_points = p_BQi_set.cols();
  DRAKE_THROW_UNLESS(p_WQi_set != nullptr);
  DRAKE_THROW_UNLESS(p_WQi_set->cols() == num_points);
  DRAKE_THROW_UNLESS(Jv_WQi != nullptr);
  DRAKE_THROW_UNLESS(Jv_WQi->rows() == 3 * num_points);
  DRAKE_THROW_UNLESS(Jv_WQi->cols() == num_velocities());

  // Compute p_WQi for each point Qi in the set P_BQi_set.
  CalcPointsPositions(context,
                      frame_B, p_BQi_set,        /* From frame B */
                      world_frame(), p_WQi_set); /* To world frame W */

  CalcPointsGeometricJacobianExpressedInWorld(
      context, frame_B, *p_WQi_set, Jv_WQi);
}

template <typename T>
VectorX<T> MultibodyTree<T>::CalcBiasForPointsGeometricJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const Frame<T>& frame_F,
    const Eigen::Ref<const MatrixX<T>>& p_FQ_list) const {
  DRAKE_THROW_UNLESS(p_FQ_list.rows() == 3);

  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);

  // For a frame F instantaneously moving with a body frame B, the spatial
  // acceleration of the frame F shifted to frame Fq with origin at point Q
  // fixed in frame F, can be computed as:
  //   A_WFq = Jv_WFq⋅v̇ + Ab_WFq,
  // where Jv_WFq is the geometric Jacobian of frame Fq and Ab_WFq is the bias
  // term for that Jacobian, defined as Ab_WFq = J̇v_WFq⋅v. The bias terms
  // contains the Coriolis and centrifugal contributions to the total spatial
  // acceleration due to non-zero velocities. Therefore, the bias term for
  // Jv_WFq is the spatial acceleration of Fq when v̇ = 0, that is:
  //   Ab_WFq = A_WFq(q, v, v̇ = 0)
  // Given the position p_BQ_W of point Q on body frame B, we can compute the
  // spatial acceleration Ab_WFq from the body spatial acceleration A_WB by
  // simply performing a shift operation:
  //   Ab_WFq = A_WB.Shift(p_BQ_W, w_WB)
  // where the shift operation also includes the angular velocity w_WB of B in
  // W since rigid shifts on acceleration will usually include additional
  // centrifugal and Coriolis terms, see SpatialAcceleration::Shift() for a
  // detailed derivation of these terms.

  // TODO(amcastro-tri): Consider caching Ab_WB(q, v), the bias term for each
  // body, and compute the bias as Ab_WBq = Ab_WB.Shift(p_BQ_W, w_WB).
  // Where the body bias terms is defined s.t. A_WB = J_WB⋅v̇ + Ab_WB or,
  // Ab_WB = J̇_WB⋅v

  std::vector<SpatialAcceleration<T>> A_WB_array(num_bodies());
  const VectorX<T> vdot = VectorX<T>::Zero(num_velocities());
  CalcSpatialAccelerationsFromVdot(context, pc, vc, vdot, &A_WB_array);

  const Body<T>& body_B = frame_F.body();
  // Bias for body B spatial acceleration.
  const SpatialAcceleration<T>& Ab_WB = A_WB_array[body_B.node_index()];

  const int num_points = p_FQ_list.cols();

  // Allocate output vector.
  VectorX<T> Ab_WB_array(3 * num_points);

  for (int ipoint = 0; ipoint < num_points; ++ipoint) {
    const Vector3<T> p_FQi = p_FQ_list.col(ipoint);

    // Body B's orientation.
    const Matrix3<T>& R_WB = pc.get_X_WB(body_B.node_index()).linear();

    // We need to compute p_BQi_W, the position of Qi in B, expressed in W.
    const Isometry3<T> X_BF = frame_F.GetFixedPoseInBodyFrame();
    const Vector3<T> p_BQi = X_BF * p_FQi;
    const Vector3<T> p_BQi_W = R_WB * p_BQi;

    // Body B's velocity in the world frame W.
    const Vector3<T>& w_WB = vc.get_V_WB(body_B.node_index()).rotational();

    // Shift body B's bias term to point Qi.
    const SpatialAcceleration<T> Ab_WBq = Ab_WB.Shift(p_BQi_W, w_WB);

    // Output translational component only.
    Ab_WB_array.template segment<3>(3 * ipoint) = Ab_WBq.translational();
  }

  return Ab_WB_array;
}

template <typename T>
void MultibodyTree<T>::CalcPointsGeometricJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const Frame<T>& frame_F, const Eigen::Ref<const MatrixX<T>>& p_WQ_list,
    EigenPtr<MatrixX<T>> Jv_WFq) const {
  DRAKE_THROW_UNLESS(p_WQ_list.rows() == 3);
  const int num_points = p_WQ_list.cols();
  DRAKE_THROW_UNLESS(Jv_WFq != nullptr);
  DRAKE_THROW_UNLESS(Jv_WFq->rows() == 3 * num_points);
  DRAKE_THROW_UNLESS(Jv_WFq->cols() == num_velocities());
  CalcFrameJacobianExpressedInWorld(
      context, frame_F, p_WQ_list,
      nullptr /* angular terms not needed */, Jv_WFq);
}

template <typename T>
void MultibodyTree<T>::CalcFrameGeometricJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const Frame<T>& frame_F, const Eigen::Ref<const Vector3<T>>& p_FQ,
    EigenPtr<MatrixX<T>> Jv_WFq) const {
  DRAKE_THROW_UNLESS(Jv_WFq != nullptr);
  DRAKE_THROW_UNLESS(Jv_WFq->rows() == 6);
  DRAKE_THROW_UNLESS(Jv_WFq->cols() == num_velocities());

  // Compute the position of Fq's origin Q in the world frame.
  Vector3<T> p_WoQ_W;
  CalcPointsPositions(context,
                      frame_F, p_FQ,             /* From frame F */
                      world_frame(), &p_WoQ_W);  /* To world frame W */

  auto Jv_WFq_angular = Jv_WFq->template topRows<3>();
  auto Jv_WFq_translational = Jv_WFq->template bottomRows<3>();

  CalcFrameJacobianExpressedInWorld(
      context, frame_F, p_WoQ_W,
      &Jv_WFq_angular, &Jv_WFq_translational);
}

template <typename T>
Vector6<T> MultibodyTree<T>::CalcBiasForFrameGeometricJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const Frame<T>& frame_F, const Eigen::Ref<const Vector3<T>>& p_FQ) const {
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);

  // For a frame F moving instantaneously with its body frame B, the spatial
  // acceleration of the frame F shifted to frame Fq with origin at point Q
  // fixed in frame F, can be computed as:
  //   A_WFq = Jv_WFq⋅v̇ + Ab_WFq,
  // where Jv_WFq is the frame geometric Jacobian for frame Fq and Ab_WFq is the
  // bias term for that Jacobian, defined as Ab_WFq = J̇v_WFq⋅v. The bias term
  // contains the Coriolis and centrifugal contributions to the total spatial
  // acceleration due to non-zero velocities. Therefore, the bias term for
  // Jv_WFq is the spatial acceleration of Fq when v̇ = 0, that is:
  //   Ab_WFq = A_WFq(q, v, v̇ = 0)
  // Given the position p_BQ_W of point Q in body frame B, we can compute the
  // spatial acceleration Ab_WFq from the body spatial acceleration A_WB by
  // simply performing a shift operation:
  //   Ab_WFq = A_WB.Shift(p_BQ_W, w_WB)
  // where the shift operation also includes the angular velocity w_WB of B in
  // W since rigid shifts on acceleration will usually include additional
  // centrifugal and Coriolis terms, see SpatialAcceleration::Shift() for a
  // detailed derivation of these terms.

  // TODO(amcastro-tri): Consider caching Ab_WB(q, v), the bias term for each
  // body, and compute the bias as Ab_WBq = Ab_WB.Shift(p_BQ_W, w_WB).
  // Where the body bias terms is defined s.t. A_WB = J_WB⋅v̇ + Ab_WB or,
  // Ab_WB = J̇_WB⋅v

  std::vector<SpatialAcceleration<T>> A_WB_array(num_bodies());
  const VectorX<T> vdot = VectorX<T>::Zero(num_velocities());
  CalcSpatialAccelerationsFromVdot(context, pc, vc, vdot, &A_WB_array);

  const Body<T>& body_B = frame_F.body();
  // Bias for body B spatial acceleration.
  const SpatialAcceleration<T>& Ab_WB = A_WB_array[body_B.node_index()];

  // Body B's orientation.
  const Matrix3<T>& R_WB = pc.get_X_WB(body_B.node_index()).linear();

  // We need to compute p_BoQ_W, the position of Q from B's origin Bo,
  // expressed in W.
  const Isometry3<T> X_BF = frame_F.GetFixedPoseInBodyFrame();
  const Vector3<T> p_BQ = X_BF * p_FQ;
  const Vector3<T> p_BQ_W = R_WB * p_BQ;

  // Body B's velocity in the world frame W.
  const Vector3<T>& w_WB = vc.get_V_WB(body_B.node_index()).rotational();

  // Shift body B's bias term to frame Q.
  const SpatialAcceleration<T> Ab_WQ = Ab_WB.Shift(p_BQ_W, w_WB);

  return Ab_WQ.get_coeffs();
}

template <typename T>
void MultibodyTree<T>::CalcFrameJacobianExpressedInWorld(
    const systems::Context<T>& context,
    const Frame<T>& frame_F,
    const Eigen::Ref<const MatrixX<T>>& p_WQ_list,
    EigenPtr<MatrixX<T>> Jw_WFq, EigenPtr<MatrixX<T>> Jv_WFq) const {
  // The user must request at least one of the terms.
  DRAKE_THROW_UNLESS(Jw_WFq != nullptr || Jv_WFq != nullptr);

  // If non-nullptr, check the proper size of the output Jacobian matrices.
  if (Jw_WFq) {
    DRAKE_THROW_UNLESS(Jw_WFq->rows() == 3);
    DRAKE_THROW_UNLESS(Jw_WFq->cols() == num_velocities());
  }
  const int num_points = p_WQ_list.cols();
  const int Jv_nrows = 3 * num_points;
  if (Jv_WFq) {
    DRAKE_THROW_UNLESS(Jv_WFq->rows() == Jv_nrows);
    DRAKE_THROW_UNLESS(Jv_WFq->cols() == num_velocities());
  }

  // If a user is re-using one of these Jacobians within a loop the first thing
  // we'll want to do is to re-initialize it to zero.
  if (Jw_WFq) Jw_WFq->setZero();
  if (Jv_WFq) Jv_WFq->setZero();

  // Body to which frame F is attached to:
  const Body<T>& body_B = frame_F.body();

  // Do nothing for bodies anchored to the world and return zero Jacobians.
  // That is, Jw_WQi * v = 0 and Jv_WQi * v = 0, always, for anchored bodies.
  if (body_B.index() == world_index()) return;

  // Compute kinematic path from body B to the world:
  std::vector<BodyNodeIndex> path_to_world;
  topology_.GetKinematicPathToWorld(body_B.node_index(), &path_to_world);

  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);

  // TODO(amcastro-tri): Eval H_PB_W from the cache.
  std::vector<Vector6<T>> H_PB_W_cache(num_velocities());
  CalcAcrossNodeGeometricJacobianExpressedInWorld(context, pc, &H_PB_W_cache);

  // Performs a scan of all bodies in the kinematic path from the world to
  // body_B, computing each node's contribution to the Jacobians.
  // Skip the world (ilevel = 0).
  for (size_t ilevel = 1; ilevel < path_to_world.size(); ++ilevel) {
    BodyNodeIndex body_node_index = path_to_world[ilevel];
    const BodyNode<T>& node = *body_nodes_[body_node_index];
    const BodyNodeTopology& node_topology = node.get_topology();
    const int start_index_in_v = node_topology.mobilizer_velocities_start_in_v;
    const int num_velocities = node_topology.num_mobilizer_velocities;

    // Across-node Jacobian.
    Eigen::Map<const MatrixUpTo6<T>> H_PB_W =
        node.GetJacobianFromArray(H_PB_W_cache);

    // Aliases to angular and translational components in H_PB_W:
    const auto Hw_PB_W = H_PB_W.template topRows<3>();
    const auto Hv_PB_W = H_PB_W.template bottomRows<3>();

    // The angular term is the same for all points since the angular
    // velocity of frame Fq, obtained by shifting frame F to origin at point Q,
    // is the same as that of frame F, for all point Q in the input list.
    if (Jw_WFq) {
      // Output block corresponding to the contribution of the mobilities in
      // level ilevel to the angular Jacobian Jw_WFq.
      auto Jw_PFq_W = Jw_WFq->block(0, start_index_in_v, 3, num_velocities);

      // Note: w_PFq_W = w_PF_W = w_PB_W.
      Jw_PFq_W = Hw_PB_W;
    }

    if (Jv_WFq) {
      // Output block corresponding to mobilities in the current node.
      // This correspond to the geometric Jacobian to compute the translational
      // velocity of frame Fq (sama as that of point Q) measured in the inboard
      // body frame P and expressed in world. That is, v_PQ_W = v_PFq_W =
      // Jv_PFq_W * v(B), with v(B) the mobilities that correspond to the
      // current node.
      auto Jv_PFq_W =
          Jv_WFq->block(0, start_index_in_v, Jv_nrows, num_velocities);

      // Position of this node's body Bi in the world W.
      const Vector3<T>& p_WBi = pc.get_X_WB(node.index()).translation();

      for (int ipoint = 0; ipoint < num_points; ++ipoint) {
        const Vector3<T>& p_WQ = p_WQ_list.col(ipoint);

        // Position of point Q measured from Bi, expressed in the world W.
        const Vector3<T> p_BiQ_W = p_WQ - p_WBi;

        // We stack the Jacobian for each translational velocity in the same
        // order the input points Q are provided in the input list.
        const int ipoint_row = 3 * ipoint;

        // Mutable alias into J_PFq_W for the translational terms for the
        // ipoint-th point.
        auto Hv_PFqi_W = Jv_PFq_W.block(ipoint_row, 0, 3, num_velocities);

        // Now "shift" H_PB_W to H_PBqi_W.
        // We do it by shifting one column at a time:
        // Note: V_PFq_W equals V_PBq_W since F moves with B.
        Hv_PFqi_W = Hv_PB_W + Hw_PB_W.colwise().cross(p_BiQ_W);
      }  // ipoint.
    }
  }  // body_node_index
}

template <typename T>
T MultibodyTree<T>::CalcPotentialEnergy(
    const systems::Context<T>& context) const {
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  return DoCalcPotentialEnergy(context, pc);
}

template <typename T>
T MultibodyTree<T>::DoCalcPotentialEnergy(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc) const {
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  T potential_energy = 0.0;
  // Add contributions from force elements.
  for (const auto& force_element : owned_force_elements_) {
    potential_energy += force_element->CalcPotentialEnergy(mbt_context, pc);
  }
  return potential_energy;
}

template <typename T>
T MultibodyTree<T>::CalcConservativePower(
    const systems::Context<T>& context) const {
  const PositionKinematicsCache<T>& pc = EvalPositionKinematics(context);
  const VelocityKinematicsCache<T>& vc = EvalVelocityKinematics(context);
  return DoCalcConservativePower(context, pc, vc);
}

template <typename T>
T MultibodyTree<T>::DoCalcConservativePower(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    const VelocityKinematicsCache<T>& vc) const {
  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  T conservative_power = 0.0;
  // Add contributions from force elements.
  for (const auto& force_element : owned_force_elements_) {
    conservative_power +=
        force_element->CalcConservativePower(mbt_context, pc, vc);
  }
  return conservative_power;
}

template <typename T>
void MultibodyTree<T>::ThrowIfFinalized(const char* source_method) const {
  if (topology_is_valid()) {
    throw std::logic_error(
        "Post-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; calls to this method must happen before Finalize().");
  }
}

template <typename T>
void MultibodyTree<T>::ThrowIfNotFinalized(const char* source_method) const {
  if (!topology_is_valid()) {
    throw std::logic_error(
        "Pre-finalize calls to '" + std::string(source_method) + "()' are "
        "not allowed; you must call Finalize() first.");
  }
}

template <typename T>
void MultibodyTree<T>::CalcArticulatedBodyInertiaCache(
    const systems::Context<T>& context,
    const PositionKinematicsCache<T>& pc,
    ArticulatedBodyInertiaCache<T>* abc) const {
  DRAKE_DEMAND(abc != nullptr);

  const auto& mbt_context =
      dynamic_cast<const MultibodyTreeContext<T>&>(context);

  // TODO(bobbyluig): Eval H_PB_W from the cache.
  std::vector<Vector6<T>> H_PB_W_cache(num_velocities());
  CalcAcrossNodeGeometricJacobianExpressedInWorld(context, pc, &H_PB_W_cache);

  // Perform tip-to-base recursion, skipping the world.
  for (int depth = tree_height() - 1; depth > 0; depth--) {
    for (BodyNodeIndex body_node_index : body_node_levels_[depth]) {
      const BodyNode<T>& node = *body_nodes_[body_node_index];

      // Get hinge mapping matrix.
      const MatrixUpTo6<T> H_PB_W = node.GetJacobianFromArray(H_PB_W_cache);

      node.CalcArticulatedBodyInertiaCache_TipToBase(
          mbt_context, pc, H_PB_W, abc);
    }
  }
}

template <typename T>
MatrixX<double> MultibodyTree<T>::MakeStateSelectorMatrix(
    const std::vector<JointIndex>& user_to_joint_index_map) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();

  // We create a set in order to verify that joint indexes appear only once.
  std::unordered_set<JointIndex> already_selected_joints;
  for (const auto& joint_index : user_to_joint_index_map) {
    const bool inserted = already_selected_joints.insert(joint_index).second;
    if (!inserted) {
      throw std::logic_error(
          "Joint named '" + get_joint(joint_index).name() +
              "' is repeated multiple times.");
    }
  }

  // Determine the size of the vector of "selected" states xₛ.
  int num_selected_positions = 0;
  int num_selected_velocities = 0;
  for (JointIndex joint_index : user_to_joint_index_map) {
    num_selected_positions += get_joint(joint_index).num_positions();
    num_selected_velocities += get_joint(joint_index).num_velocities();
  }
  const int num_selected_states =
      num_selected_positions + num_selected_velocities;

  // With state x of size n and selected state xₛ of size nₛ, Sx has size
  // nₛ x n so that xₛ = Sx⋅x.
  MatrixX<double> Sx =
      MatrixX<double>::Zero(num_selected_states, num_states());

  const int nq = num_positions();
  // We place all selected positions first, followed by all the selected
  // velocities, as in the original state x.
  int selected_positions_index = 0;
  int selected_velocities_index = num_selected_positions;
  for (JointIndex joint_index : user_to_joint_index_map) {
    const auto& joint = get_joint(joint_index);

    const int pos_start = joint.position_start();
    const int num_pos = joint.num_positions();
    const int vel_start = joint.velocity_start();
    const int num_vel = joint.num_velocities();

    Sx.block(selected_positions_index, pos_start, num_pos, num_pos) =
        MatrixX<double>::Identity(num_pos, num_pos);

    Sx.block(selected_velocities_index, nq + vel_start, num_vel, num_vel) =
        MatrixX<double>::Identity(num_vel, num_vel);

    selected_positions_index += num_pos;
    selected_velocities_index += num_vel;
  }

  return Sx;
}

template <typename T>
MatrixX<double> MultibodyTree<T>::MakeStateSelectorMatrixFromJointNames(
    const std::vector<std::string>& selected_joints) const {
  std::vector<JointIndex> selected_joints_indexes;
  for (const auto& joint_name : selected_joints) {
    selected_joints_indexes.push_back(GetJointByName(joint_name).index());
  }
  return MakeStateSelectorMatrix(selected_joints_indexes);
}

template <typename T>
MatrixX<double> MultibodyTree<T>::MakeActuatorSelectorMatrix(
    const std::vector<JointActuatorIndex>& user_to_actuator_index_map) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();

  const int num_selected_actuators = user_to_actuator_index_map.size();

  // The actuation selector matrix maps the vector of "selected" actuators to
  // the full vector of actuators: u = Sᵤ⋅uₛ.
  MatrixX<double> Su =
      MatrixX<double>::Zero(num_actuated_dofs(), num_selected_actuators);
  int user_index = 0;
  for (JointActuatorIndex actuator_index : user_to_actuator_index_map) {
    Su(actuator_index, user_index) = 1.0;
    ++user_index;
  }

  return Su;
}

template <typename T>
MatrixX<double> MultibodyTree<T>::MakeActuatorSelectorMatrix(
    const std::vector<JointIndex>& user_to_joint_index_map) const {
  DRAKE_MBT_THROW_IF_NOT_FINALIZED();

  std::vector<JointActuatorIndex> joint_to_actuator_index(num_joints());
  for (JointActuatorIndex actuator_index(0);
       actuator_index < num_actuators(); ++actuator_index) {
    const auto& actuator = get_joint_actuator(actuator_index);
    joint_to_actuator_index[actuator.joint().index()] = actuator_index;
  }

  // Build a list of actuators in the order given by user_to_joint_index_map,
  // which must contain actuated joints. We verify this.
  std::vector<JointActuatorIndex> user_to_actuator_index_map;
  for (JointIndex joint_index : user_to_joint_index_map) {
    const auto& joint = get_joint(joint_index);

    // If the map has an invalid index then this joint does not have an
    // actuator.
    if (!joint_to_actuator_index[joint_index].is_valid()) {
      throw std::logic_error(
          "Joint '" + joint.name() + "' does not have an actuator.");
    }

    user_to_actuator_index_map.push_back(joint_to_actuator_index[joint_index]);
  }

  return MakeActuatorSelectorMatrix(user_to_actuator_index_map);
}

// Explicitly instantiates on the most common scalar types.
template class MultibodyTree<double>;
template class MultibodyTree<AutoDiffXd>;

}  // namespace multibody
}  // namespace drake
