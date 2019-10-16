/*
 * Copyright (C) 2017-2019 Trent Houliston <trent@houliston.me>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <tensorflow/core/framework/op.h>
#include <tensorflow/core/framework/op_kernel.h>
#include <tensorflow/core/framework/shape_inference.h>

#include <memory>
#include <mutex>

#include "engine/cpu/cpu_engine.hpp"
#include "geometry/Circle.hpp"
#include "geometry/Sphere.hpp"
#include "mesh/mesh.hpp"

REGISTER_OP("VisualMesh")
  .Attr("T: {float, double}")
  .Attr("U: {int32, int64}")
  .Input("image_dimensions: U")
  .Input("lens_type: string")
  .Input("lens_focal_length: T")
  .Input("lens_fov: T")
  .Input("lens_centre: T")
  .Input("cam_to_observation_plane: T")
  .Input("height: T")
  .Input("n_intersections: T")
  .Input("cached_meshes: int32")
  .Input("intersection_tolerance: T")
  .Input("max_distance: T")
  .Input("geometry: string")
  .Input("radius: T")
  .Output("pixels: T")
  .Output("neighbours: int32")
  .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* c) {
    // nx2 points on image, and n+1x7 neighbours (including off screen point)
    c->set_output(0, c->MakeShape({c->kUnknownDim, 2}));
    c->set_output(1, c->MakeShape({c->kUnknownDim, c->kUnknownDim}));
    return tensorflow::Status::OK();
  });

enum Args {
  DIMENSIONS             = 0,
  PROJECTION             = 1,
  FOCAL_LENGTH           = 2,
  FIELD_OF_VIEW          = 3,
  LENS_CENTRE            = 4,
  ROC                    = 5,
  HEIGHT                 = 6,
  N_INTERSECTIONS        = 7,
  CACHED_MESHES          = 8,
  INTERSECTION_TOLERANCE = 9,
  MAX_DISTANCE           = 10,
  GEOMETRY               = 11,
  RADIUS                 = 12
};

/**
 * @brief Given a shape, two heights and a k value, calculate the absolute number of intersections difference given the
 *        new height.
 *
 * @tparam Scalar the scalar type used for calculations and storage (normally one of float or double)
 * @tparam Shape  the type of shape to use when calculating the error
 *
 * @param shape the instance of the shape that will be used to calculate the k error
 * @param h_0   the height of the camera in the mesh we are comparing to
 * @param h_1   the current height of the camera we want to get an error for
 * @param k     the k value that the original mesh was designed to use
 *
 * @return what the k value would be if we used this mesh at this height
 */
template <typename Scalar, template <typename> class Shape>
Scalar mesh_k_error(const Shape<Scalar>& shape, const Scalar& h_0, const Scalar& h_1, const Scalar& k) {
  return std::abs(k - k * shape.k(h_0, h_1));
}

/**
 * @brief Lookup an appropriate Visual Mesh to use for this lens and height given the provided tolerances
 *
 * @tparam Scalar the scalar type used for calculations and storage (normally one of float or double)
 * @tparam Shape  the type of shape to use when calculating the error
 *
 * @param meshes  the list of meshes that we will be looking for the target in
 * @param shape   the shape that we will be using for the lookup
 * @param h       the current height of the camera above the ground
 * @param k       the number of cross sectional intersections that we want with the object
 * @param t       the tolerance for the number of cross sectional intersections before we need a new mesh
 * @param d       the maximum distance that the mesh should be generated for
 *
 * @return either returns a shared_ptr to the mesh that would best fit within our tolerance, or if none could be found a
 *         nullptr
 */
template <typename Scalar, template <typename> class Generator, template <typename> class Shape>
std::shared_ptr<visualmesh::Mesh<Scalar, Generator>> find_mesh(
  std::vector<std::shared_ptr<visualmesh::Mesh<Scalar, Generator>>>& meshes,
  const Shape<Scalar>& shape,
  const Scalar& h,
  const Scalar& k,
  const Scalar& t,
  const Scalar& d) {

  // Nothing in the map!
  if (meshes.empty()) { return nullptr; }

  // Find the best mesh we have available
  auto best_it      = meshes.begin();
  Scalar best_error = std::numeric_limits<Scalar>::max();
  for (auto it = meshes.begin(); it != meshes.end(); ++it) {
    auto error = mesh_k_error(shape, (*it)->h, h, k);
    if (d == (*it)->max_distance && error < best_error) {
      best_error = error;
      best_it    = it;
    }
  }

  // Swap it to the top of the list so we can keep somewhat of which items are most used
  std::iter_swap(meshes.begin(), best_it);

  // If it was good enough return it, otherwise return null
  return best_error <= t ? *best_it : nullptr;
};

/**
 * @brief Lookup or create an appropriate Visual Mesh to use for this lens and height given the provided tolerances
 *
 * @details
 *  This function gets the best fitting mesh that it can find that is within the number of intersections tolerance. If
 *  it cannot find a mesh that matches the tolerance it will create a new one for the provided details. The mesh will
 *  not match if the maximum distance has changed, only if the k difference is small enough. Additionally it will only
 *  cache `cached_meshes` number of meshes. Each time a mesh is used again it will move it to the top of the list, and
 *  if a new mesh must be added and this would exceed this limit the least recently used mesh will be dropped.
 *
 * @tparam Scalar the scalar type used for calculations and storage (normally one of float or double)
 * @tparam Shape  the type of shape to use when calculating the error
 *
 * @param shape                   the shape that we will be using for the lookup
 * @param height                  the current height of the camera above the ground
 * @param n_intersections         the number of cross sectional intersections that we want with the object
 * @param intersection_tolerance  tolerance for the number of cross sectional intersections before we need a new mesh
 * @param cached_meshes           the number of meshes to cache at any one time before we delete one
 * @param max_distance            the maximum distance that the mesh should be generated for
 *
 * @return std::shared_ptr<visualmesh::Mesh<Scalar>>
 */
template <typename Scalar, template <typename> class Generator, template <typename> class Shape>
std::shared_ptr<visualmesh::Mesh<Scalar, Generator>> get_mesh(const Shape<Scalar>& shape,
                                                              const Scalar& height,
                                                              const Scalar& n_intersections,
                                                              const Scalar& intersection_tolerance,
                                                              const int32_t& cached_meshes,
                                                              const Scalar& max_distance) {

  // Static map of heights to meshes
  static std::vector<std::shared_ptr<visualmesh::Mesh<Scalar, Generator>>> meshes;
  static std::mutex mesh_mutex;

  // Find and return an element if one is appropriate
  /* mutex scope */ {
    std::lock_guard<std::mutex> lock(mesh_mutex);

    // If we found an acceptable mesh return it
    auto mesh = find_mesh(meshes, shape, height, n_intersections, intersection_tolerance, max_distance);
    if (mesh != nullptr) { return mesh; }
  }

  // We couldn't find an appropriate mesh, make a new one but don't hold the mutex while we do so others can still query
  auto new_mesh = std::make_shared<visualmesh::Mesh<Scalar, Generator>>(shape, height, n_intersections, max_distance);

  /* mutex scope */ {
    std::lock_guard<std::mutex> lock(mesh_mutex);

    // Check again for an acceptable mesh in case someone else made one too
    auto mesh = find_mesh(meshes, shape, height, n_intersections, intersection_tolerance, max_distance);
    if (mesh != nullptr) { return mesh; }
    // Otherwise add the one we made to the list
    else {

      // Only cache a fixed number of meshes so remove the old ones
      while (static_cast<int32_t>(meshes.size()) > cached_meshes) {
        meshes.pop_back();
      }

      // Add our new mesh to the cache and return
      meshes.push_back(new_mesh);
      return new_mesh;
    }
  }
}

/**
 * @brief The Visual Mesh tensorflow op
 *
 * @details
 *  This op will perform a projection using the visual mesh and will return the neighbourhood graph and the pixel
 * coordinates for the points that would be on screen for the lens paramters provided.
 *
 * @tparam T The scalar type used for floating point numbers
 * @tparam U The scalar type used for integer numbers
 */
template <typename T, typename U>
class VisualMeshOp : public tensorflow::OpKernel {
public:
  template <typename V>
  using Generator = visualmesh::generator::QuadPizza<V>;

  explicit VisualMeshOp(tensorflow::OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(tensorflow::OpKernelContext* context) override {

    // Check that the shape of each of the inputs is valid
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsVector(context->input(Args::DIMENSIONS).shape())
                  && context->input(Args::DIMENSIONS).shape().dim_size(0) == 2,
                tensorflow::errors::InvalidArgument("The image dimensions must be a 2d vector of [y_size, x_size]"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::FOCAL_LENGTH).shape()),
                tensorflow::errors::InvalidArgument("The focal length must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::FIELD_OF_VIEW).shape()),
                tensorflow::errors::InvalidArgument("The field of view must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsVector(context->input(Args::LENS_CENTRE).shape())
                  && context->input(Args::LENS_CENTRE).shape().dim_size(0) == 2,
                tensorflow::errors::InvalidArgument("The lens centre must be a 2d vector of [y_size, x_size]"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsSquareMatrix(context->input(Args::ROC).shape())
                  && context->input(Args::ROC).shape().dim_size(0) == 3,
                tensorflow::errors::InvalidArgument("Roc must be a 3x3 matrix"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::HEIGHT).shape()),
                tensorflow::errors::InvalidArgument("The height must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::N_INTERSECTIONS).shape()),
                tensorflow::errors::InvalidArgument("The number of intersections must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::CACHED_MESHES).shape()),
                tensorflow::errors::InvalidArgument("The number cached meshes must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::INTERSECTION_TOLERANCE).shape()),
                tensorflow::errors::InvalidArgument("The intersection tolerance must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::MAX_DISTANCE).shape()),
                tensorflow::errors::InvalidArgument("The maximum distance must be a scalar"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::GEOMETRY).shape()),
                tensorflow::errors::InvalidArgument("Geometry must be a single string value"));
    OP_REQUIRES(context,
                tensorflow::TensorShapeUtils::IsScalar(context->input(Args::RADIUS).shape()),
                tensorflow::errors::InvalidArgument("The radius must be a scalar"));

    // Extract information from our input tensors, flip x and y as tensorflow has them reversed compared to us
    auto image_dimensions                = context->input(Args::DIMENSIONS).vec<U>();
    visualmesh::vec2<int32_t> dimensions = {{int32_t(image_dimensions(1)), int32_t(image_dimensions(0))}};
    std::string projection               = *context->input(Args::PROJECTION).flat<tensorflow::string>().data();
    T focal_length                       = context->input(Args::FOCAL_LENGTH).scalar<T>()(0);
    T fov                                = context->input(Args::FIELD_OF_VIEW).scalar<T>()(0);
    auto lens_centre                     = context->input(Args::LENS_CENTRE).flat<T>();
    auto tRoc                            = context->input(Args::ROC).matrix<T>();
    T height                             = context->input(Args::HEIGHT).scalar<T>()(0);
    T max_distance                       = context->input(Args::MAX_DISTANCE).scalar<T>()(0);
    T n_intersections                    = context->input(Args::N_INTERSECTIONS).scalar<T>()(0);
    tensorflow::int32 cached_meshes      = context->input(Args::N_INTERSECTIONS).scalar<tensorflow::int32>()(0);
    T intersection_tolerance             = context->input(Args::INTERSECTION_TOLERANCE).scalar<T>()(0);
    std::string geometry                 = *context->input(Args::GEOMETRY).flat<tensorflow::string>().data();
    T radius                             = context->input(Args::RADIUS).scalar<T>()(0);

    // Perform some runtime checks on the actual values to make sure they make sense
    OP_REQUIRES(context,
                projection == "EQUISOLID" || projection == "EQUIDISTANT" || projection == "RECTILINEAR",
                tensorflow::errors::InvalidArgument("Projection must be one of EQUISOLID, EQUIDISTANT or RECTILINEAR"));
    OP_REQUIRES(context,
                geometry == "SPHERE" || geometry == "CIRCLE",
                tensorflow::errors::InvalidArgument("Geometry must be one of SPHERE or CIRCLE"));

    // Create our transformation matrix
    visualmesh::mat4<T> Hoc = {{
      visualmesh::vec4<T>{tRoc(0, 0), tRoc(0, 1), tRoc(0, 2), 0},
      visualmesh::vec4<T>{tRoc(1, 0), tRoc(1, 1), tRoc(1, 2), 0},
      visualmesh::vec4<T>{tRoc(2, 0), tRoc(2, 1), tRoc(2, 2), height},
      visualmesh::vec4<T>{0, 0, 0, 1},
    }};

    // Create our lens
    visualmesh::Lens<T> lens;
    lens.dimensions   = dimensions;
    lens.focal_length = focal_length;
    lens.fov          = fov;
    lens.centre       = {{lens_centre(1), lens_centre(0)}};  // Swap from tf coordinates to our coordinates
    if (projection == "EQUISOLID") {
      lens.projection = visualmesh::EQUISOLID;  //
    }
    else if (projection == "EQUIDISTANT") {
      lens.projection = visualmesh::EQUIDISTANT;
    }
    else if (projection == "RECTILINEAR") {
      lens.projection = visualmesh::RECTILINEAR;
    }

    // Project the mesh using our engine and shape
    visualmesh::engine::cpu::Engine<T> engine;
    visualmesh::ProjectedMesh<T, Generator<T>::N_NEIGHBOURS> projected;

    if (geometry == "SPHERE") {
      visualmesh::geometry::Sphere<T> shape(radius);
      std::shared_ptr<visualmesh::Mesh<T, Generator>> mesh = get_mesh<T, Generator, visualmesh::geometry::Sphere>(
        shape, height, n_intersections, intersection_tolerance, cached_meshes, max_distance);
      projected = engine.project(*mesh, mesh->lookup(Hoc, lens), Hoc, lens);
    }
    else if (geometry == "CIRCLE") {
      visualmesh::geometry::Circle<T> shape(radius);
      std::shared_ptr<visualmesh::Mesh<T, Generator>> mesh = get_mesh<T, Generator, visualmesh::geometry::Circle>(
        shape, height, n_intersections, intersection_tolerance, cached_meshes, max_distance);
      projected = engine.project(*mesh, mesh->lookup(Hoc, lens), Hoc, lens);
    }

    // Get the interesting things out of the projected mesh
    const auto& px            = projected.pixel_coordinates;
    const auto& neighbourhood = projected.neighbourhood;

    // Fill in our tensorflow output matrix
    tensorflow::Tensor* coordinates = nullptr;
    tensorflow::TensorShape coords_shape;
    coords_shape.AddDim(px.size());
    coords_shape.AddDim(2);
    OP_REQUIRES_OK(context, context->allocate_output(0, coords_shape, &coordinates));

    // Copy across our pixel coordinates remembering to reverse the order from x,y to y,x
    auto c = coordinates->matrix<T>();
    for (size_t i = 0; i < px.size(); ++i) {
      // Swap x and y here since tensorflow expects them reversed
      const auto& p = px[i];
      c(i, 0)       = p[1];
      c(i, 1)       = p[0];
    }

    // Build our tensorflow neighbourhood graph
    tensorflow::Tensor* neighbours = nullptr;
    tensorflow::TensorShape neighbours_shape;
    neighbours_shape.AddDim(neighbourhood.size());
    neighbours_shape.AddDim(
      neighbourhood.front().size());  // TODO THIS WILL EXPLODE WHEN THERE ARE NO ELEMENTS ON SCREEN
    OP_REQUIRES_OK(context, context->allocate_output(1, neighbours_shape, &neighbours));

    // Copy across our neighbourhood graph, adding in a point for itself
    auto n = neighbours->matrix<U>();
    for (unsigned int i = 0; i < neighbourhood.size(); ++i) {
      // Get our old neighbours from original output
      const auto& m = neighbourhood[i];
      // First point is ourself
      n(i, 0) = i;
      for (unsigned int j = 0; j < neighbourhood[i].size(); ++j) {
        n(i, j + 1) = m[j];
      }
    }
  }
};

// Register a version for all the combinations of float/double and int32/int64
REGISTER_KERNEL_BUILDER(
  Name("VisualMesh").Device(tensorflow::DEVICE_CPU).TypeConstraint<float>("T").TypeConstraint<tensorflow::int32>("U"),
  VisualMeshOp<float, tensorflow::int32>);
REGISTER_KERNEL_BUILDER(
  Name("VisualMesh").Device(tensorflow::DEVICE_CPU).TypeConstraint<float>("T").TypeConstraint<tensorflow::int64>("U"),
  VisualMeshOp<float, tensorflow::int64>);
REGISTER_KERNEL_BUILDER(
  Name("VisualMesh").Device(tensorflow::DEVICE_CPU).TypeConstraint<double>("T").TypeConstraint<tensorflow::int32>("U"),
  VisualMeshOp<double, tensorflow::int32>);
REGISTER_KERNEL_BUILDER(
  Name("VisualMesh").Device(tensorflow::DEVICE_CPU).TypeConstraint<double>("T").TypeConstraint<tensorflow::int64>("U"),
  VisualMeshOp<double, tensorflow::int64>);
