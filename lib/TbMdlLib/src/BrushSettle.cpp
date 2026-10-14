/*
 Copyright (C) 2026 TrenchBroom contributors
 SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mdl/BrushSettle.h"

#include "mdl/BrushFace.h"

#include <algorithm>
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <random>

namespace tb::mdl
{
namespace
{
constexpr double Scale = 1.0 / 32.0;
constexpr double ContactTolerance = 0.25; // Map units; includes hull contact margins.

btVector3 toPhysics(const vm::vec3d& p)
{
  return {btScalar(p.x() * Scale), btScalar(p.y() * Scale), btScalar(p.z() * Scale)};
}

// Exact volume and centroid of a convex polyhedron by tetrahedral decomposition.
std::pair<double, vm::vec3d> volumeAndCenter(const Brush& brush)
{
  const auto origin = brush.bounds().center();
  auto volume = 0.0;
  auto weightedCenter = vm::vec3d{};
  for (const auto& face : brush.faces())
  {
    const auto points = face.vertexPositions();
    for (size_t i = 1; i + 1 < points.size(); ++i)
    {
      const auto v =
        std::abs(
          vm::dot(
            points[0] - origin, vm::cross(points[i] - origin, points[i + 1] - origin)))
        / 6.0;
      volume += v;
      weightedCenter =
        weightedCenter + (origin + points[0] + points[i] + points[i + 1]) * (v / 4.0);
    }
  }
  return {volume, volume > 0.0 ? weightedCenter / volume : origin};
}
} // namespace

struct BrushSettleSimulation::Impl
{
  btDefaultCollisionConfiguration configuration;
  btCollisionDispatcher dispatcher{&configuration};
  btDbvtBroadphase broadphase;
  btSequentialImpulseConstraintSolver solver;
  btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &configuration};
  std::unique_ptr<btTriangleMesh> patchMesh;
  std::vector<std::unique_ptr<btCollisionShape>> shapes;
  std::vector<std::unique_ptr<btRigidBody>> bodies;
  std::vector<Brush> brushes;
  std::vector<vm::vec3d> centers;
  vm::bbox3d bounds;
  BrushSettleOptions options;
  size_t frames = 0;
  bool escaped = false;

  ~Impl()
  {
    for (auto& body : bodies)
    {
      world.removeRigidBody(body.get());
    }
  }

  void addBody(const Brush& brush, const bool dynamic)
  {
    const auto [volume, center] = volumeAndCenter(brush);
    auto shape = std::make_unique<btConvexHullShape>();
    // Small positive margins stabilize contact without visibly inflating pieces.
    shape->setMargin(btScalar(0.02 * Scale));
    for (const auto& p : brush.vertexPositions())
    {
      shape->addPoint(toPhysics(p - center), false);
    }
    shape->recalcLocalAabb();
    const auto mass =
      dynamic ? btScalar(std::clamp(volume * Scale * Scale * Scale, 0.001, 10000.0))
              : btScalar(0);
    auto inertia = btVector3{0, 0, 0};
    if (dynamic)
    {
      shape->calculateLocalInertia(mass, inertia);
      centers.push_back(center);
    }
    auto info =
      btRigidBody::btRigidBodyConstructionInfo{mass, nullptr, shape.get(), inertia};
    info.m_friction = btScalar(options.friction);
    info.m_restitution = btScalar(options.restitution);
    info.m_linearDamping = btScalar(0.12);
    info.m_angularDamping = btScalar(0.25);
    auto body = std::make_unique<btRigidBody>(info);
    auto transform = btTransform::getIdentity();
    transform.setOrigin(toPhysics(center));
    body->setWorldTransform(transform);
    body->setSleepingThresholds(btScalar(0.02), btScalar(0.02));
    if (dynamic)
    {
      auto radius = brush.bounds().size().x();
      for (const auto& face : brush.faces())
      {
        radius = std::min(radius, -face.boundary().point_distance(center));
      }
      body->setCcdSweptSphereRadius(btScalar(std::max(0.001, radius * 0.8) * Scale));
      body->setCcdMotionThreshold(btScalar(std::max(0.001, radius * 0.5) * Scale));
    }
    world.addRigidBody(body.get());
    shapes.push_back(std::move(shape));
    bodies.push_back(std::move(body));
  }

  bool penetrating()
  {
    world.performDiscreteCollisionDetection();
    for (int i = 0; i < dispatcher.getNumManifolds(); ++i)
    {
      const auto* manifold = dispatcher.getManifoldByIndexInternal(i);
      if (
        manifold->getBody0()->isStaticObject() && manifold->getBody1()->isStaticObject())
      {
        continue;
      }
      for (int j = 0; j < manifold->getNumContacts(); ++j)
      {
        if (manifold->getContactPoint(j).getDistance() < -ContactTolerance * Scale)
        {
          return true;
        }
      }
    }
    return false;
  }
};

BrushSettleSimulation::BrushSettleSimulation(std::unique_ptr<Impl> impl)
  : m_impl{std::move(impl)}
{
}
BrushSettleSimulation::~BrushSettleSimulation() = default;

Result<std::unique_ptr<BrushSettleSimulation>> BrushSettleSimulation::create(
  std::vector<Brush> brushes,
  const std::vector<Brush>& obstacles,
  const vm::bbox3d& worldBounds,
  const BrushSettleOptions& options,
  const std::vector<SettleTriangle>& patchTriangles)
{
  if (brushes.empty() || brushes.size() > MaxBodies || obstacles.size() > MaxObstacles)
  {
    return Error{
      "Select 1-128 brushes; at most 4096 visible obstacle brushes are supported."};
  }
  if (
    !std::isfinite(options.friction) || options.friction < 0 || options.friction > 1
    || !std::isfinite(options.restitution) || options.restitution < 0
    || options.restitution > 1 || !std::isfinite(options.kick) || options.kick < 0
    || options.kick > 256 || !std::isfinite(options.duration) || options.duration < 0.1
    || options.duration > 20)
  {
    return Error{"Invalid settle parameters."};
  }
  auto impl = std::make_unique<Impl>();
  impl->brushes = std::move(brushes);
  impl->bounds = worldBounds;
  impl->options = options;
  impl->world.setGravity(btVector3{0, 0, btScalar(-800 * Scale)});
  impl->world.getSolverInfo().m_numIterations = 40;
  impl->world.getSolverInfo().m_splitImpulse = true;
  impl->world.getSolverInfo().m_splitImpulsePenetrationThreshold = 0;
  impl->world.getSolverInfo().m_solverMode &= ~SOLVER_RANDMIZE_ORDER;
  // Insert dynamic bodies first; transforms and result preserve selection order.
  for (const auto& brush : impl->brushes)
  {
    if (
      !brush.closed() || !brush.fullySpecified() || volumeAndCenter(brush).first < 0.001)
    {
      return Error{"A selected brush has no usable closed volume."};
    }
    impl->addBody(brush, true);
  }
  for (const auto& obstacle : obstacles)
  {
    impl->addBody(obstacle, false);
  }
  if (patchTriangles.size() > 262144)
  {
    return Error{"Too many patch triangles. Hide unrelated patches before using Settle."};
  }
  if (!patchTriangles.empty())
  {
    impl->patchMesh = std::make_unique<btTriangleMesh>();
    for (const auto& triangle : patchTriangles)
    {
      for (const auto& p : triangle)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()))
          return Error{"Invalid patch triangle."};
      if (
        vm::squared_length(
          vm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]))
        > 1e-12)
        impl->patchMesh->addTriangle(
          toPhysics(triangle[0]), toPhysics(triangle[1]), toPhysics(triangle[2]));
    }
    if (impl->patchMesh->getNumTriangles() > 0)
    {
      auto shape = std::make_unique<btBvhTriangleMeshShape>(impl->patchMesh.get(), true);
      shape->setMargin(btScalar(0.02 * Scale));
      auto info = btRigidBody::btRigidBodyConstructionInfo{0, nullptr, shape.get()};
      info.m_friction = btScalar(options.friction);
      info.m_restitution = btScalar(options.restitution);
      auto body = std::make_unique<btRigidBody>(info);
      impl->world.addRigidBody(body.get());
      impl->shapes.push_back(std::move(shape));
      impl->bodies.push_back(std::move(body));
    }
  }
  if (impl->penetrating())
  {
    return Error{
      "Selected brushes overlap each other or the scenery. Separate them before "
      "simulating."};
  }
  auto random = std::mt19937{options.seed};
  const auto signedUnit = [&] {
    return 2.0 * (double(random()) + 0.5) / 4294967296.0 - 1.0;
  };
  for (size_t i = 0; i < impl->brushes.size(); ++i)
  {
    auto& body = *impl->bodies[i];
    const auto x = signedUnit();
    const auto y = signedUnit();
    const auto z = std::abs(signedUnit());
    body.setLinearVelocity(toPhysics(vm::vec3d{x, y, z} * options.kick));
    const auto ax = signedUnit();
    const auto ay = signedUnit();
    const auto az = signedUnit();
    body.setAngularVelocity(
      btVector3{
        btScalar(ax * options.kick / 64),
        btScalar(ay * options.kick / 64),
        btScalar(az * options.kick / 64)});
  }
  return std::unique_ptr<BrushSettleSimulation>{
    new BrushSettleSimulation{std::move(impl)}};
}

std::vector<vm::mat4x4d> BrushSettleSimulation::transforms() const
{
  auto result = std::vector<vm::mat4x4d>{};
  for (size_t i = 0; i < m_impl->brushes.size(); ++i)
  {
    const auto& transform = m_impl->bodies[i]->getWorldTransform();
    auto matrix = vm::mat4x4d{};
    for (size_t c = 0; c < 3; ++c)
    {
      for (size_t r = 0; r < 3; ++r)
      {
        matrix[c][r] = double(transform.getBasis()[int(r)][int(c)]);
      }
    }
    const auto& p = transform.getOrigin();
    const auto delta = vm::vec3d{double(p.x()), double(p.y()), double(p.z())} / Scale
                       - matrix * m_impl->centers[i];
    for (size_t r = 0; r < 3; ++r)
    {
      matrix[3][r] = delta[r];
    }
    result.push_back(matrix);
  }
  return result;
}

void BrushSettleSimulation::step()
{
  if (finished())
  {
    return;
  }
  for (int i = 0; i < 4; ++i)
  {
    m_impl->world.stepSimulation(btScalar(1.0 / 240.0), 0);
  }
  ++m_impl->frames;
  const auto matrices = transforms();
  for (size_t i = 0; i < matrices.size(); ++i)
  {
    for (const auto& vertex : m_impl->brushes[i].vertexPositions())
    {
      const auto p = matrices[i] * vertex;
      if (
        !std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z())
        || !m_impl->bounds.contains(p))
      {
        m_impl->escaped = true;
      }
    }
  }
}

bool BrushSettleSimulation::sleeping() const
{
  for (size_t i = 0; i < m_impl->brushes.size(); ++i)
  {
    if (m_impl->bodies[i]->isActive())
    {
      return false;
    }
  }
  return true;
}
bool BrushSettleSimulation::finished() const
{
  return m_impl->escaped || elapsed() >= m_impl->options.duration || sleeping();
}
double BrushSettleSimulation::elapsed() const
{
  return double(m_impl->frames) / 60.0;
}

Result<std::vector<Brush>> BrushSettleSimulation::result()
{
  if (m_impl->escaped)
  {
    return Error{
      "A fragment left the map bounds. Add a floor or reduce the duration or kick."};
  }
  if (m_impl->penetrating())
  {
    return Error{"Fragments still intersect. Resume the simulation or try another seed."};
  }
  auto brushes = m_impl->brushes;
  const auto matrices = transforms();
  for (size_t i = 0; i < brushes.size(); ++i)
  {
    if (!brushes[i].transform(m_impl->bounds, matrices[i], true))
    {
      return Error{
        "A rotated fragment became invalid. Try another seed or stop earlier."};
    }
  }
  return brushes;
}
} // namespace tb::mdl
