// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "../common/default.h"
#include "../common/device.h"
#include "../common/scene.h"
#include "../common/context.h"
#include "../../include/embree4/rtcore.h"

#include "../geometry/disc_intersector.h"
#include "../geometry/sphere_intersector.h"
#include "../geometry/roundline_intersector.h"
#include "../geometry/curve_intersector_ribbon.h"
#include "../geometry/curve_intersector_oriented.h"
#include "../geometry/curve_intersector_sweep.h"
#include "../geometry/curve_intersector_distance.h"
#include "../geometry/curve_intersector_ribbon.h"
#include "../geometry/curveNi_intersector.h"
#include "../geometry/instance_intersector.h"
#include "../geometry/intersector_epilog_sycl.h"
#include "../geometry/triangle_intersector_moeller.h"
#include "../geometry/triangle_intersector_pluecker.h"

#include "builder/qbvh6.h"
#include "rthwif_embree.h"
#include "rthwif_production.h"
using namespace embree;

RTC_NAMESPACE_BEGIN;

//#if defined(EMBREE_DPCPP_ROBUST)
#define TriangleIntersector isa::PlueckerIntersector1<1,true>
#define ROBUST_MODE true
//#else
//#define TriangleIntersector isa::MoellerTrumboreIntersector1<1,true>
//#define ROBUST_MODE false
//#endif

#undef TRAV_LOOP
#if (RTC_MAX_INSTANCE_LEVEL_COUNT > 1) ||\
    defined(EMBREE_DPCPP_MBLUR)       ||\
    defined(EMBREE_GEOMETRY_CURVE)    ||\
    defined(EMBREE_GEOMETRY_GRID)     ||\
    defined(EMBREE_GEOMETRY_POINT)    ||\
    defined(EMBREE_GEOMETRY_USER)     ||\
    defined(EMBREE_FILTER_FUNCTION)
#define TRAV_LOOP
#endif

const constexpr uint32_t TRAV_LOOP_FEATURES =
#if (RTC_MAX_INSTANCE_LEVEL_COUNT > 1)
  RTC_FEATURE_TRIANGLE |   // filter function enforced for triangles and quads in this case
  RTC_FEATURE_QUAD |
#endif
  RTC_FEATURE_MOTION_BLUR |
  RTC_FEATURE_ROUND_CURVES | RTC_FEATURE_FLAT_CURVES | RTC_FEATURE_NORMAL_ORIENTED_CURVES |
  RTC_FEATURE_GRID |
  RTC_FEATURE_POINT |
  RTC_FEATURE_USER_GEOMETRY |
  RTC_FEATURE_FILTER_FUNCTION;

void use_rthwif_embree() {
}

Vec3f intel_get_hit_triangle_normal(intel_ray_query_t* query, intel_hit_type_t hit_type)
{
  float3 v[3]; intel_get_hit_triangle_verts(query, v, hit_type);
  const Vec3f v0(v[0].x(), v[0].y(), v[0].z());
  const Vec3f v1(v[1].x(), v[1].y(), v[1].z());
  const Vec3f v2(v[2].x(), v[2].y(), v[2].z());
  return cross(v1-v0, v2-v0);
}

bool intersect_user_geometry(intel_ray_query_t* query, RayHit& ray, UserGeometry* geom, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], sycl::private_ptr<IntersectContext> context, uint32_t geomID, uint32_t primID)
{
  /* perform ray mask test */
#if defined(EMBREE_RAY_MASK)
  if ((ray.mask & geom->mask) == 0)
    return false;
#endif

  RTCScene forward_scene = nullptr;
  const bool ishit = geom->intersect(ray,geomID,primID,context,forward_scene);
  if (!forward_scene) return ishit;

  /* forward ray to instanced scene */
  unsigned int bvh_level = intel_get_hit_bvh_level( query, HIT_TYPE_INTEL_POTENTIAL_HIT ) + 1;
  Scene* scene = (Scene*) forward_scene;
  scenes[bvh_level] = scene;
  
  intel_ray_desc_t raydesc;
  raydesc.O = float3(ray.org.x, ray.org.y, ray.org.z);
  raydesc.D = float3(ray.dir.x, ray.dir.y, ray.dir.z);
  raydesc.tmin = ray.tnear();
  raydesc.tmax = inf; // unused
  raydesc.time = 0.0f;
  raydesc.mask = mask32_to_mask8(ray.mask);
  raydesc.flags = RAY_FLAGS_INTEL_FORCE_NON_OPAQUE;
  // FIXME: how to forward ray time here

#if defined(EMBREE_BACKFACE_CULLING)
  raydesc.flags |= RAY_FLAGS_INTEL_CULL_BACK_FACING_TRIANGLES;
#endif

  intel_raytracing_acceleration_structure_t* hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) scene->hwaccel.data();

  QBVH6* qbvh6 = (QBVH6*) hwaccel_ptr;
  void** AccelTable = (void**) (qbvh6+1);
  hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) AccelTable[0];
    
  intel_ray_query_forward_ray(query, bvh_level, raydesc, hwaccel_ptr, 0);
  return false;
}

bool intersect_user_geometry(intel_ray_query_t* query, Ray& ray, UserGeometry* geom, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], sycl::private_ptr<IntersectContext> context, uint32_t geomID, uint32_t primID)
{
  /* perform ray mask test */
#if defined(EMBREE_RAY_MASK)
  if ((ray.mask & geom->mask) == 0) 
    return false;
#endif

  RTCScene forward_scene = nullptr;
  const bool ishit = geom->occluded(ray,geomID,primID,context,forward_scene);
  if (!forward_scene) return ishit;

  /* forward ray to instanced scene */
  unsigned int bvh_level = intel_get_hit_bvh_level( query, HIT_TYPE_INTEL_POTENTIAL_HIT ) + 1;
  Scene* scene = (Scene*) forward_scene;
  scenes[bvh_level] = scene;
  
  intel_ray_desc_t raydesc;
  raydesc.O = float3(ray.org.x, ray.org.y, ray.org.z);
  raydesc.D = float3(ray.dir.x, ray.dir.y, ray.dir.z);
  raydesc.tmin = ray.tnear();
  raydesc.tmax = inf; // unused
  raydesc.time = 0.0f;
  raydesc.mask = mask32_to_mask8(ray.mask);
  raydesc.flags = RAY_FLAGS_INTEL_FORCE_NON_OPAQUE | RAY_FLAGS_INTEL_ACCEPT_FIRST_HIT_AND_END_SEARCH;
  // FIXME: how to forward ray time here

#if defined(EMBREE_BACKFACE_CULLING)
  raydesc.flags |= RAY_FLAGS_INTEL_CULL_BACK_FACING_TRIANGLES;
#endif

  intel_raytracing_acceleration_structure_t* hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) scene->hwaccel.data();

  QBVH6* qbvh6 = (QBVH6*) hwaccel_ptr;
  void** AccelTable = (void**) (qbvh6+1);
  hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) AccelTable[0];

  intel_ray_query_forward_ray(query, bvh_level, raydesc, hwaccel_ptr, 0);
  return false;
}

template<typename Ray>
bool intersect_instance(intel_ray_query_t* query, Ray& ray, Instance* instance, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], sycl::private_ptr<IntersectContext> context, uint32_t geomID, uint32_t primID);

template<>
bool intersect_instance(intel_ray_query_t* query, RayHit& ray, Instance* instance, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], sycl::private_ptr<IntersectContext> context, uint32_t geomID, uint32_t primID)
{
  /* perform ray mask test */
#if defined(EMBREE_RAY_MASK)
  if ((ray.mask & instance->mask) == 0) 
    return false;
#endif

  if (!instance_id_stack::push(context->user, geomID))
    return false;

  unsigned int bvh_level = intel_get_hit_bvh_level( query, HIT_TYPE_INTEL_POTENTIAL_HIT ) + 1;

  Scene* object = (Scene*) instance->object;
  const AffineSpace3fa world2local = instance->getWorld2Local(ray.time());
  const Vec3fa ray_org = xfmPoint (world2local, (Vec3f) ray.org);
  const Vec3fa ray_dir = xfmVector(world2local, (Vec3f) ray.dir);
  scenes[bvh_level] = object;
  
  intel_ray_desc_t raydesc;
  raydesc.O = float3(ray_org.x, ray_org.y, ray_org.z);
  raydesc.D = float3(ray_dir.x, ray_dir.y, ray_dir.z);
  raydesc.tmin = ray.tnear();
  raydesc.tmax = inf; // unused
  raydesc.time = 0.0f;
  raydesc.mask = mask32_to_mask8(ray.mask);
  raydesc.flags = RAY_FLAGS_INTEL_FORCE_NON_OPAQUE;
  
#if defined(EMBREE_BACKFACE_CULLING)
  raydesc.flags |= RAY_FLAGS_INTEL_CULL_BACK_FACING_TRIANGLES;
#endif

  intel_raytracing_acceleration_structure_t* hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) object->hwaccel.data();
  
  uint32_t bvh_id = 0;
  QBVH6* qbvh6 = (QBVH6*) hwaccel_ptr;
#if defined(EMBREE_DPCPP_MBLUR)
  float time = clamp(ray.time(),0.0f,1.0f);
  bvh_id = (uint32_t) clamp(uint32_t(qbvh6->numTimeSegments*time), 0u, qbvh6->numTimeSegments-1);
#endif

  void** AccelTable = (void**) (qbvh6+1);
  hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) AccelTable[bvh_id];
  bvh_id = 0;
  
  intel_ray_query_forward_ray(query, bvh_level, raydesc, hwaccel_ptr, bvh_id);

  return false;
}

template<>
bool intersect_instance(intel_ray_query_t* query, Ray& ray, Instance* instance, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], sycl::private_ptr<IntersectContext> context, uint32_t geomID, uint32_t primID)
{
  /* perform ray mask test */
#if defined(EMBREE_RAY_MASK)
  if ((ray.mask & instance->mask) == 0) 
    return false;
#endif

  if (!instance_id_stack::push(context->user, geomID))
    return false;

  unsigned int bvh_level = intel_get_hit_bvh_level( query, HIT_TYPE_INTEL_POTENTIAL_HIT ) + 1;

  Scene* object = (Scene*) instance->object;
  const AffineSpace3fa world2local = instance->getWorld2Local(ray.time());
  const Vec3fa ray_org = xfmPoint (world2local, (Vec3f) ray.org);
  const Vec3fa ray_dir = xfmVector(world2local, (Vec3f) ray.dir);
  scenes[bvh_level] = object;
  
  intel_ray_desc_t raydesc;
  raydesc.O = float3(ray_org.x, ray_org.y, ray_org.z);
  raydesc.D = float3(ray_dir.x, ray_dir.y, ray_dir.z);
  raydesc.tmin = ray.tnear();
  raydesc.tmax = inf; // unused
  raydesc.time = 0.0f;
  raydesc.mask = mask32_to_mask8(ray.mask);
  raydesc.flags = RAY_FLAGS_INTEL_ACCEPT_FIRST_HIT_AND_END_SEARCH;
  
#if defined(EMBREE_BACKFACE_CULLING)
  raydesc.flags |= RAY_FLAGS_INTEL_CULL_BACK_FACING_TRIANGLES;
#endif

  intel_raytracing_acceleration_structure_t* hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) object->hwaccel.data();
  
  uint32_t bvh_id = 0;
  QBVH6* qbvh6 = (QBVH6*) hwaccel_ptr;
#if defined(EMBREE_DPCPP_MBLUR)
  float time = clamp(ray.time(),0.0f,1.0f);
  bvh_id = (uint32_t) clamp(uint32_t(qbvh6->numTimeSegments*time), 0u, qbvh6->numTimeSegments-1);
#endif

  void** AccelTable = (void**) (qbvh6+1);
  hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) AccelTable[bvh_id];
  bvh_id = 0;

  intel_ray_query_forward_ray(query, bvh_level, raydesc, hwaccel_ptr, bvh_id);

  return false;
}

template<typename Ray>
bool intersect_primitive(intel_ray_query_t* query, Ray& ray, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], Geometry* geom, sycl::private_ptr<IntersectContext> context, uint32_t geomID, uint32_t primID, const RTCFeatureFlags feature_mask)
{
#if defined(__SYCL_DEVICE_ONLY__)
  bool filter = feature_mask & (RTC_FEATURE_FILTER_FUNCTION_IN_CONTEXT | RTC_FEATURE_FILTER_FUNCTION_IN_GEOMETRY);
#if defined(EMBREE_DPCPP_MBLUR)
  if (feature_mask & RTC_FEATURE_MOTION_BLUR) {
    if (ray.time() < geom->time_range.lower || geom->time_range.upper < ray.time())
      return false;
  }
#endif

#if defined(EMBREE_GEOMETRY_USER)
  if (geom->getType() == Geometry::GTY_USER_GEOMETRY) {
    return intersect_user_geometry(query,ray,(UserGeometry*)geom, scenes, context, geomID, primID);
  }
#endif

#if defined(EMBREE_GEOMETRY_INSTANCE)
#if (RTC_MAX_INSTANCE_LEVEL_COUNT >= 2) || defined(EMBREE_DPCPP_MBLUR)
  if (geom->getTypeMask() & Geometry::MTY_INSTANCE) {
    return intersect_instance(query,ray,(Instance*)geom, scenes, context, geomID, primID);
  }
#endif
#endif
  
  isa::CurvePrecalculations1 pre(ray,context->scene);
  
  const Geometry::GType gtype MAYBE_UNUSED = geom->getType();
  const Geometry::GType stype MAYBE_UNUSED = (Geometry::GType)(gtype & Geometry::GTY_SUBTYPE_MASK);
  const Geometry::GType basis MAYBE_UNUSED = (Geometry::GType)(gtype & Geometry::GTY_BASIS_MASK);

#if defined(EMBREE_DPCPP_MBLUR)
#if defined(EMBREE_GEOMETRY_TRIANGLE)
  if (gtype == Geometry::GTY_TRIANGLE_MESH && (feature_mask & RTC_FEATURE_TRIANGLE) && (feature_mask & RTC_FEATURE_MOTION_BLUR))
  {
    const TriangleMesh* geom = context->scene->get<TriangleMesh>(geomID);
    const TriangleMesh::Triangle triangle = geom->triangle(primID);
    Vec3fa v0 = geom->vertex(triangle.v[0], ray.time());
    Vec3fa v1 = geom->vertex(triangle.v[1], ray.time());
    Vec3fa v2 = geom->vertex(triangle.v[2], ray.time());
    return TriangleIntersector().intersect(ray,v0,v1,v2,Intersect1Epilog1_HWIF<Ray>(ray, context, geomID, primID, filter));
  } else
#endif

#if defined(EMBREE_GEOMETRY_QUAD)
  if (gtype == Geometry::GTY_QUAD_MESH && (feature_mask & RTC_FEATURE_QUAD) && (feature_mask & RTC_FEATURE_MOTION_BLUR))
  {
    const QuadMesh* geom = context->scene->get<QuadMesh>(geomID);
    const QuadMesh::Quad quad = geom->quad(primID);
    Vec3fa v0 = geom->vertex(quad.v[0], ray.time());
    Vec3fa v1 = geom->vertex(quad.v[1], ray.time());
    Vec3fa v2 = geom->vertex(quad.v[2], ray.time());
    Vec3fa v3 = geom->vertex(quad.v[3], ray.time());
    bool ishit0 = TriangleIntersector().intersect(ray,v0,v1,v3,Intersect1Epilog1_HWIF<Ray>(ray, context, geomID, primID, filter));
    bool ishit1 = TriangleIntersector().intersect(ray,v2,v3,v1,[&](float &u, float &v, Vec3f& Ng) { u = 1.f - u; v = 1.f - v; }, Intersect1Epilog1_HWIF<Ray>(ray, context, geomID, primID, filter));
    return ishit0 || ishit1;
  } else
#endif
#endif

#if defined(EMBREE_GEOMETRY_GRID)
  if (gtype == Geometry::GTY_GRID_MESH && (feature_mask & RTC_FEATURE_GRID))
  {
    const GridMesh* mesh = context->scene->get<GridMesh>(geomID);
    const GridMesh::PrimID_XY c = mesh->quadID_to_primID_xy[primID];
    const GridMesh::Grid& g = mesh->grid(c.primID);

    auto map_uv0 = [&](float &u, float &v, Vec3f& Ng) {
      const Vec2f uv(u,v);
      u = (c.x + uv.x)/(g.resX-1);
      v = (c.y + uv.y)/(g.resY-1);
    };

    auto map_uv1 = [&](float &u, float &v, Vec3f& Ng) {
      const Vec2f uv(1.0f-u, 1.0f-v);
      u = (c.x + uv.x)/(g.resX-1);
      v = (c.y + uv.y)/(g.resY-1);
    };

    Vec3fa v0,v1,v2,v3;
    mesh->gather_quad_vertices_safe(v0,v1,v2,v3,g,c.x,c.y,ray.time());

    bool ishit0 = TriangleIntersector().intersect(ray,v0,v1,v3,map_uv0,Intersect1Epilog1_HWIF<Ray>(ray, context, geomID, c.primID, filter));
    bool ishit1 = TriangleIntersector().intersect(ray,v2,v3,v1,map_uv1,Intersect1Epilog1_HWIF<Ray>(ray, context, geomID, c.primID, filter));
    return ishit0 || ishit1;
  } else
#endif

#if defined(EMBREE_GEOMETRY_POINT)
  
  if (gtype == Geometry::GTY_SPHERE_POINT && (feature_mask & RTC_FEATURE_SPHERE_POINT))
  {
    const Points* geom = context->scene->get<Points>(geomID);
    const Vec3ff xyzr = geom->vertex_safe(primID, ray.time());
    const Vec4f vr(xyzr.x,xyzr.y,xyzr.z,xyzr.w);
    return isa::SphereIntersector1<1>::intersect(true, ray, pre, vr, Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
  }
  else if (gtype == Geometry::GTY_DISC_POINT && (feature_mask & RTC_FEATURE_DISC_POINT))
  {
    const Points* geom = context->scene->get<Points>(geomID);
    const Vec3ff xyzr = geom->vertex_safe(primID, ray.time());
    const Vec4f vr(xyzr.x,xyzr.y,xyzr.z,xyzr.w);
    return isa::DiscIntersector1<1>::intersect(true, ray, nullptr, nullptr, pre, vr, Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
  }
  else if (gtype == Geometry::GTY_ORIENTED_DISC_POINT && (feature_mask & RTC_FEATURE_ORIENTED_DISC_POINT))
  {
    const Points* geom = context->scene->get<Points>(geomID);
    const Vec3ff xyzr = geom->vertex_safe(primID, ray.time());
    const Vec4f vr(xyzr.x,xyzr.y,xyzr.z,xyzr.w);
    const Vec3f n = geom->normal_safe(primID, ray.time());
    return isa::DiscIntersector1<1>::intersect(true, ray, nullptr, nullptr, pre, vr, n, Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
  } else
    
#endif

#if defined(EMBREE_GEOMETRY_CURVE)

  if (geom->getTypeMask() & Geometry::MTY_CURVES)
  {
    if (gtype == Geometry::GTY_FLAT_LINEAR_CURVE && (feature_mask & RTC_FEATURE_FLAT_LINEAR_CURVE))
    {
      LineSegments* geom = context->scene->get<LineSegments>(geomID);
      Vec3ff v0, v1; geom->gather_safe(v0,v1,geom->segment(primID),ray.time());
      return isa::FlatLinearCurveIntersector1<1>::intersect(true,ray,context,geom,pre,v0,v1,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
    }
    else if (gtype == Geometry::GTY_ROUND_LINEAR_CURVE && (feature_mask & RTC_FEATURE_ROUND_LINEAR_CURVE))
    {
      LineSegments* geom = context->scene->get<LineSegments>(geomID);
      Vec3ff v0,v1,v2,v3; geom->gather_safe(v0,v1,v2,v3,primID,geom->segment(primID),ray.time());
      return isa::RoundLinearCurveIntersector1<1>().intersect(true,ray,context,geom,pre,v0,v1,v2,v3,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
    }
    else if (gtype == Geometry::GTY_CONE_LINEAR_CURVE && (feature_mask & RTC_FEATURE_CONE_LINEAR_CURVE))
    {
      LineSegments* geom = context->scene->get<LineSegments>(geomID);
      Vec3ff v0 = zero, v1 = zero; bool cL = false, cR = false;
      geom->gather_safe(v0,v1,cL,cR,primID,geom->segment(primID),ray.time());
      return isa::ConeCurveIntersector1<1>().intersect(true,ray,context,geom,pre,v0,v1,cL,cR,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
    }
    else
    {
      CurveGeometry* geom = context->scene->get<CurveGeometry>(geomID);
      if (stype == Geometry::GTY_SUBTYPE_ORIENTED_CURVE && (feature_mask & RTC_FEATURE_NORMAL_ORIENTED_CURVES))
      {
        using Intersector = isa::OrientedCurve1Intersector1<CubicBezierCurve,8,1>;
        using Curve = isa::TensorLinearCubicBezierSurface3fa;
        if (geom->numTimeSegments() > 0 && (feature_mask & RTC_FEATURE_MOTION_BLUR))
        {
          Curve curve;
          if (basis == Geometry::GTY_BASIS_HERMITE && (feature_mask & RTC_FEATURE_NORMAL_ORIENTED_HERMITE_CURVE)) {
            curve = geom->getNormalOrientedHermiteCurveSafe<HermiteCurveT<Vec3ff>, HermiteCurveT<Vec3fa>, Curve>(context,ray.org,primID,ray.time());
          }
          else if (basis == Geometry::GTY_BASIS_BSPLINE && (feature_mask & RTC_FEATURE_NORMAL_ORIENTED_BSPLINE_CURVE)) {
            curve = geom->getNormalOrientedCurveSafe<BSplineCurveT<Vec3ff>, BSplineCurveT<Vec3fa>, Curve>(context,ray.org,primID,ray.time());
          }
          else if (basis == Geometry::GTY_BASIS_CATMULL_ROM && (feature_mask & RTC_FEATURE_NORMAL_ORIENTED_CATMULL_ROM_CURVE)) {
            curve = geom->getNormalOrientedCurveSafe<CatmullRomCurveT<Vec3ff>, CatmullRomCurveT<Vec3fa>, Curve>(context,ray.org,primID,ray.time());
          }
          else {
            curve = geom->getNormalOrientedCurveSafe<Intersector::SourceCurve3ff, Intersector::SourceCurve3fa, Curve>(context,ray.org,primID,ray.time());
          }
          return Intersector().intersect(pre,ray,context,geom,primID,curve,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
        }
        else
        {
          Vec3ff v0,v1,v2,v3;
          Vec3fa n0,n1,n2,n3;
          if (basis == Geometry::GTY_BASIS_HERMITE && (feature_mask & RTC_FEATURE_NORMAL_ORIENTED_HERMITE_CURVE))
            geom->gather_hermite_safe(v0,v1,n0,n1,v2,v3,n2,n3,geom->curve(primID),ray.time());
          else
            geom->gather_safe(v0,v1,v2,v3,n0,n1,n2,n3,geom->curve(primID),ray.time());
          isa::convert_to_bezier(gtype, v0,v1,v2,v3, n0,n1,n2,n3);
          return Intersector().intersect(pre,ray,context,geom,primID,v0,v1,v2,v3,n0,n1,n2,n3,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
        }
      }
      else if (feature_mask & (RTC_FEATURE_FLAT_CURVES | RTC_FEATURE_ROUND_CURVES)) {
        Vec3ff v0,v1,v2,v3;
        if (basis == Geometry::GTY_BASIS_HERMITE && (feature_mask & (RTC_FEATURE_ROUND_HERMITE_CURVE | RTC_FEATURE_FLAT_HERMITE_CURVE)))
          geom->gather_hermite_safe(v0,v1,v2,v3,geom->curve(primID),ray.time());
        else
          geom->gather_safe(v0,v1,v2,v3,geom->curve(primID),ray.time());
        
        isa::convert_to_bezier(gtype, v0,v1,v2,v3);

        if (stype == Geometry::GTY_SUBTYPE_FLAT_CURVE && (feature_mask & RTC_FEATURE_FLAT_CURVES))
        {
          isa::RibbonCurve1Intersector1<CubicBezierCurve,1> intersector;
          return intersector.intersect(pre,ray,context,geom,primID,v0,v1,v2,v3,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
        }
        else if (stype == Geometry::GTY_SUBTYPE_ROUND_CURVE && (feature_mask & RTC_FEATURE_ROUND_CURVES))
        {
          isa::SweepCurve1Intersector1<CubicBezierCurve> intersector;
          return intersector.intersect(pre,ray,context,geom,primID,v0,v1,v2,v3,Intersect1Epilog1_HWIF<Ray>(ray,context,geomID,primID,filter));
        }
        return false;
      }
      return false;
    }
  } else
    
#endif

#endif
  
  return false;
}

bool invokeTriangleIntersectionFilter(intel_ray_query_t* query, Geometry* geom, uint32_t bvh_level, RayHit& ray, Hit& hit, sycl::private_ptr<IntersectContext> context, const RTCFeatureFlags feature_mask)
{
#if defined(EMBREE_FILTER_FUNCTION)
  if (!(feature_mask & RTC_FEATURE_FILTER_FUNCTION) || runIntersectionFilter1SYCL(geom, ray, context, hit))
#endif
  {
    intel_ray_query_commit_potential_hit (query, ray.tfar, float2(hit.u, hit.v));
    
    for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l)
      ray.instID[l] = hit.instID[l];
  }
  return false;
}

bool invokeTriangleIntersectionFilter(intel_ray_query_t* query, Geometry* geom, uint32_t bvh_level, Ray& ray, Hit& hit, sycl::private_ptr<IntersectContext> context, const RTCFeatureFlags feature_mask)
{
  bool ishit = true;
#if defined(EMBREE_FILTER_FUNCTION)
  ishit = !(feature_mask & RTC_FEATURE_FILTER_FUNCTION) || runIntersectionFilter1SYCL(geom, ray, context, hit);
  if (ishit)
#endif
  {
    intel_ray_query_commit_potential_hit (query, ray.tfar, float2(hit.u, hit.v));
  }
  return ishit;
}

bool commit_potential_hit(intel_ray_query_t* query, RayHit& ray) {
  intel_ray_query_commit_potential_hit (query, ray.tfar, float2(ray.u, ray.v));
  return false;
}

bool commit_potential_hit(intel_ray_query_t* query, Ray& ray) {
  intel_ray_query_commit_potential_hit (query, ray.tfar, float2(0.0f, 0.0f));
  return true;
}

#if defined(TRAV_LOOP)
template<typename Ray>
void trav_loop(intel_ray_query_t* query, Ray& ray, Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1], sycl::private_ptr<IntersectContext> context, const RTCFeatureFlags feature_mask)
{
  while (!intel_is_traversal_done(query))
  {
    intel_candidate_type_t candidate = intel_get_hit_candidate(query, HIT_TYPE_INTEL_POTENTIAL_HIT);

    const unsigned int bvh_level = intel_get_hit_bvh_level( query, HIT_TYPE_INTEL_POTENTIAL_HIT );
    const float3 org = intel_get_ray_origin   ( query, bvh_level );
    const float3 dir = intel_get_ray_direction( query, bvh_level );
    const float t = intel_get_hit_distance(query, HIT_TYPE_INTEL_POTENTIAL_HIT);
    const float2 uv = intel_get_hit_barys (query, HIT_TYPE_INTEL_POTENTIAL_HIT);
    const unsigned int geomID = intel_get_hit_geomID(query, HIT_TYPE_INTEL_POTENTIAL_HIT);
    const unsigned int primID = intel_get_hit_primID(query, HIT_TYPE_INTEL_POTENTIAL_HIT);

    ray.org = Vec3ff(org.x(), org.y(), org.z(), ray.tnear());
    ray.dir = Vec3ff(dir.x(), dir.y(), dir.z(), ray.time ());
    ray.tfar = intel_get_hit_distance(query, HIT_TYPE_INTEL_COMMITTED_HIT);
   
#if RTC_MAX_INSTANCE_LEVEL_COUNT > 1
    context->user->instStackSize = bvh_level;
    Scene* scene = scenes[bvh_level];
#else
    const unsigned int instID = intel_get_hit_instID(query, HIT_TYPE_INTEL_POTENTIAL_HIT);
    
    /* assume software instancing mode by default (required for rtcForwardRay) */
    Scene* scene = scenes[bvh_level]; 

    /* if we are in hardware instancing mode and we need to read the scene from the instance */
    if (bvh_level > 0 && instID != RTC_INVALID_GEOMETRY_ID) {
      Instance* inst = scenes[0]->get<Instance>(instID);
      scene = (Scene*) inst->object;
      context->user->instID[0] = instID;
    }
    else if (bvh_level == 0)
      context->user->instID[0] = RTC_INVALID_GEOMETRY_ID;
    
#endif
    context->scene = scene;
    Geometry* geom = scene->get(geomID);

    /* perform ray masking */
    if (ray.mask & geom->mask)
    {
      if (candidate == CANDIDATE_TYPE_INTEL_PROCEDURAL)
      {
        if (intersect_primitive(query,ray,scenes,geom,context,geomID,primID,feature_mask))
          if (commit_potential_hit (query, ray))
            break; // shadow rays break at first hit

      }
      else // if (candidate == TRIANGLE)
      {
        ray.tfar = t;
        Vec3f Ng = intel_get_hit_triangle_normal(query, HIT_TYPE_INTEL_POTENTIAL_HIT);
        Hit hit(context->user,geomID,primID,Vec2f(uv.x(),uv.y()),Ng);
        if (invokeTriangleIntersectionFilter(query, geom, bvh_level, ray, hit, context, feature_mask))
          break; // shadow rays break at first hit
      }
    }

    intel_ray_query_start_traversal(query);
    intel_ray_query_sync(query);
  }
}
#endif

SYCL_EXTERNAL void rtcIntersectRTHW(sycl::global_ptr<RTCSceneTy> hscene, sycl::private_ptr<RTCIntersectContext> ucontext, sycl::private_ptr<RTCRayHit> rayhit_i, sycl::private_ptr<RTCIntersectArguments> args)
{
  Scene* scene = (Scene*) hscene.get();
  intel_raytracing_acceleration_structure_t* hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) scene->hwaccel.data();

  Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1];
  scenes[0] = scene;

  IntersectContext context(scene, ucontext, args);

  RayHit ray;
  ray.org = Vec3ff(rayhit_i->ray.org_x, rayhit_i->ray.org_y, rayhit_i->ray.org_z, rayhit_i->ray.tnear);
  ray.dir = Vec3ff(rayhit_i->ray.dir_x, rayhit_i->ray.dir_y, rayhit_i->ray.dir_z, rayhit_i->ray.time);
  ray.tfar = rayhit_i->ray.tfar;
  ray.mask = rayhit_i->ray.mask;
  ray.id = rayhit_i->ray.id;
  ray.flags = rayhit_i->ray.flags;
  ray.Ng = Vec3f(0,0,0);
  ray.u = 0;
  ray.v = 0;
  ray.primID = RTC_INVALID_GEOMETRY_ID;
  ray.geomID = RTC_INVALID_GEOMETRY_ID;
  
#if RTC_MAX_INSTANCE_LEVEL_COUNT > 1
  for (uint32_t l=0; l<RTC_MAX_INSTANCE_LEVEL_COUNT; l++)
    ray.instID[l] = RTC_INVALID_GEOMETRY_ID;
#else
  ray.instID[0] = RTC_INVALID_GEOMETRY_ID;
#endif
  
  intel_ray_desc_t raydesc;
  raydesc.O = float3(ray.org.x, ray.org.y, ray.org.z);
  raydesc.D = float3(ray.dir.x, ray.dir.y, ray.dir.z);
  raydesc.tmin = ray.tnear();
  raydesc.tmax = ray.tfar;
  raydesc.time = 0.0f;
  raydesc.mask = mask32_to_mask8(ray.mask);
  raydesc.flags = RAY_FLAGS_INTEL_NONE;
  
#if RTC_MAX_INSTANCE_LEVEL_COUNT > 1
  raydesc.flags |= RAY_FLAGS_INTEL_FORCE_NON_OPAQUE;
#endif

#if defined(EMBREE_BACKFACE_CULLING)
  raydesc.flags |= RAY_FLAGS_INTEL_CULL_BACK_FACING_TRIANGLES;
#endif

  uint32_t bvh_id = 0;
  QBVH6* qbvh6 = (QBVH6*) hwaccel_ptr;
#if defined(EMBREE_DPCPP_MBLUR)
  if(args->feature_mask & RTC_FEATURE_MOTION_BLUR) {
    float time = clamp(ray.time(),0.0f,1.0f);
    bvh_id = (uint32_t) clamp(uint32_t(qbvh6->numTimeSegments*time), 0u, qbvh6->numTimeSegments-1);
  }
#endif

  void** AccelTable = (void**) (qbvh6+1);
  hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) AccelTable[bvh_id];
  bvh_id = 0;
  
  intel_ray_query_t query_ = intel_ray_query_init(0, raydesc, hwaccel_ptr, bvh_id);
  intel_ray_query_t* query = &query_;
  
  intel_ray_query_start_traversal(query);
  intel_ray_query_sync(query);
  
#if defined(TRAV_LOOP)
  if (args->feature_mask & TRAV_LOOP_FEATURES) {
    trav_loop(query,ray,scenes,&context,args->feature_mask);
  }
#endif

  bool valid = intel_has_committed_hit(query);

  if (valid)
  {
    unsigned int bvh_level = intel_get_hit_bvh_level(query, HIT_TYPE_INTEL_COMMITTED_HIT);
    float t = intel_get_hit_distance(query, HIT_TYPE_INTEL_COMMITTED_HIT);
    float2 uv = intel_get_hit_barys (query, HIT_TYPE_INTEL_COMMITTED_HIT);
    unsigned int geomID = intel_get_hit_geomID(query, HIT_TYPE_INTEL_COMMITTED_HIT);
    unsigned int instID = intel_get_hit_instID(query, HIT_TYPE_INTEL_COMMITTED_HIT);

    unsigned int primID = ray.primID;
    if (intel_get_hit_candidate(query, HIT_TYPE_INTEL_COMMITTED_HIT) == CANDIDATE_TYPE_INTEL_TRIANGLE)
      primID = intel_get_hit_primID_triangle(query, HIT_TYPE_INTEL_COMMITTED_HIT);
      
    rayhit_i->ray.tfar = t;
    rayhit_i->hit.geomID = geomID;
    rayhit_i->hit.primID = primID;
    rayhit_i->hit.u = uv.x();
    rayhit_i->hit.v = uv.y();
    
#if RTC_MAX_INSTANCE_LEVEL_COUNT > 1
    for (uint32_t l=0; l<RTC_MAX_INSTANCE_LEVEL_COUNT; l++)
      rayhit_i->hit.instID[l] = ray.instID[l];
#else
    /* when rtcForwardRay was used then we are in software instancing mode */
    if (bvh_level > 0 && instID == RTC_INVALID_GEOMETRY_ID)
      instID = ray.instID[0];
    
    rayhit_i->hit.instID[0] = instID;
#endif

    /* calculate geometry normal for hardware accelerated triangles */
    if (intel_get_hit_candidate(query, HIT_TYPE_INTEL_COMMITTED_HIT) == CANDIDATE_TYPE_INTEL_TRIANGLE)
      ray.Ng = intel_get_hit_triangle_normal(query, HIT_TYPE_INTEL_COMMITTED_HIT);

    rayhit_i->hit.Ng_x = ray.Ng.x;
    rayhit_i->hit.Ng_y = ray.Ng.y;
    rayhit_i->hit.Ng_z = ray.Ng.z;
  }
  else
  {
    rayhit_i->hit.geomID = -1;
  }
}

SYCL_EXTERNAL void rtcOccludedRTHW(sycl::global_ptr<RTCSceneTy> hscene, sycl::private_ptr<RTCIntersectContext> ucontext, sycl::private_ptr<RTCRay> ray_i, sycl::private_ptr<RTCIntersectArguments> args)
{
#if defined(__SYCL_DEVICE_ONLY__)
  Scene* scene = (Scene*) hscene.get();
  intel_raytracing_acceleration_structure_t* hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) scene->hwaccel.data();

  Scene* scenes[RTC_MAX_INSTANCE_LEVEL_COUNT+1];
  scenes[0] = scene;
  
  IntersectContext context(scene, ucontext, args);
  
  Ray ray;
  ray.org = Vec3ff(ray_i->org_x, ray_i->org_y, ray_i->org_z, ray_i->tnear);
  ray.dir = Vec3ff(ray_i->dir_x, ray_i->dir_y, ray_i->dir_z, ray_i->time);
  ray.tfar = ray_i->tfar;
  ray.mask = ray_i->mask;
  ray.id = ray_i->id;
  ray.flags = ray_i->flags;
  
  intel_ray_desc_t raydesc;
  raydesc.O = float3(ray.org.x, ray.org.y, ray.org.z);
  raydesc.D = float3(ray.dir.x, ray.dir.y, ray.dir.z);
  raydesc.tmin = ray.tnear();
  raydesc.tmax = ray.tfar;
  raydesc.time = 0.0f;
  raydesc.mask = mask32_to_mask8(ray.mask);
  raydesc.flags = RAY_FLAGS_INTEL_ACCEPT_FIRST_HIT_AND_END_SEARCH;
  
#if defined(EMBREE_BACKFACE_CULLING)
  raydesc.flags |= RAY_FLAGS_INTEL_CULL_BACK_FACING_TRIANGLES;
#endif

  uint32_t bvh_id = 0;
  QBVH6* qbvh6 = (QBVH6*) hwaccel_ptr;
#if defined(EMBREE_DPCPP_MBLUR)
  if(args->feature_mask & RTC_FEATURE_MOTION_BLUR) {
    float time = clamp(ray.time(),0.0f,1.0f);
    bvh_id = (uint32_t) clamp(uint32_t(qbvh6->numTimeSegments*time), 0u, qbvh6->numTimeSegments-1);
  }
#endif

  void** AccelTable = (void**) (qbvh6+1);
  hwaccel_ptr = (intel_raytracing_acceleration_structure_t*) AccelTable[bvh_id];
  bvh_id = 0;
  
  intel_ray_query_t query_ = intel_ray_query_init(0, raydesc, hwaccel_ptr, bvh_id);
  intel_ray_query_t* query = &query_;
  intel_ray_query_start_traversal(query);
  intel_ray_query_sync(query);

#if defined(TRAV_LOOP)
  if (args->feature_mask & TRAV_LOOP_FEATURES) {
    trav_loop(query,ray,scenes,&context,args->feature_mask);
  }
#endif
  
  if (intel_has_committed_hit(query))
    ray_i->tfar = -INFINITY;
  
#endif
}

#undef TRAV_LOOP

RTC_NAMESPACE_END;
