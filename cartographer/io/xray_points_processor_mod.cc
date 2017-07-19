/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/io/xray_points_processor_mod.h"

#include <cmath>
#include <string>

#include "Eigen/Core"
#include "cairo/cairo.h"
#include "cartographer/common/lua_parameter_dictionary.h"
#include "cartographer/common/make_unique.h"
#include "cartographer/common/math.h"
#include "cartographer/io/cairo_types.h"
#include "cartographer/mapping/detect_floors.h"
#include "cartographer/mapping_3d/hybrid_grid.h"

namespace cartographer {
namespace io {
namespace {

struct PixelData {
  size_t num_occupied_cells_in_column = 0;
  double mean_r = 0.;
  double mean_g = 0.;
  double mean_b = 0.;
};

using PixelDataMatrix =
    Eigen::Matrix<PixelData, Eigen::Dynamic, Eigen::Dynamic>;

double Mix(const double a, const double b, const double t) {
  return a * (1. - t) + t * b;
}

cairo_status_t CairoWriteCallback(void* const closure,
                                  const unsigned char* data,
                                  const unsigned int length) {
  if (static_cast<FileWriter*>(closure)->Write(
          reinterpret_cast<const char*>(data), length)) {
    return CAIRO_STATUS_SUCCESS;
  }
  return CAIRO_STATUS_WRITE_ERROR;
}

// Write 'mat' as a pleasing-to-look-at PNG into 'filename'
void WritePng(const PixelDataMatrix& mat, FileWriter* const file_writer, double voxel_size) {
  const int stride =
      cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, mat.cols());
  CHECK_EQ(stride % 4, 0);
  std::vector<uint32_t> pixels(stride / 4 * mat.rows(), 0.);

  float max = std::numeric_limits<float>::min();
  for (int y = 0; y < mat.rows(); ++y) {
    for (int x = 0; x < mat.cols(); ++x) {
      const PixelData& cell = mat(y, x);
      if (cell.num_occupied_cells_in_column == 0.) {
        continue;
      }
      max = std::max<float>(max, cell.num_occupied_cells_in_column);
    }
  }

  for (int y = 0; y < mat.rows(); ++y) {
    for (int x = 0; x < mat.cols(); ++x) {
      const PixelData& cell = mat(y, x);
      if (cell.num_occupied_cells_in_column == 0.) {
        pixels[y * stride / 4 + x] =
            (0 << 24) | (0 << 16) | (0 << 8) | 0;
        continue;
      }

      // We use a logarithmic weighting for how saturated a pixel will be. The
      // basic idea here was that walls (full height) are fully saturated, but
      // details like chairs and tables are still well visible.
      float wall_height_voxels = 2.5 / voxel_size;  //2.5m = wall
      bool wall = (cell.num_occupied_cells_in_column / wall_height_voxels) > 0.30; //40% of wall height is wall
      bool obj = !wall && (cell.num_occupied_cells_in_column * voxel_size) > 0.2 ; // More than 0.2m occupied equals an uncertainty 

      //Default (< 0.2) is floor - green
      int r = 0, g = 255, b = 0, alpha = 255;

      if (obj)
      {
        //Range is between 0.2 and min height to be a wall
        float saturation = std::log(cell.num_occupied_cells_in_column) / std::log(0.3 * wall_height_voxels);
        std::cout <<"Num="<<cell.num_occupied_cells_in_column<<"   /   "<< 0.3 * wall_height_voxels <<"   =  "<<saturation<<std::endl;
        g = 0;
        r = 255;
        alpha = 128; //static_cast<int>(saturation * 255.);
      }
      else if (wall)
      {
        r = 0;
        g = 0;
        b = 255;
      }

      pixels[y * stride / 4 + x] = (alpha << 24) | (r << 16) | (g << 8) | b;

      int rval = (pixels[y * stride / 4 + x]  & 0x00FF0000) >> 16;
      int aval = (pixels[y * stride / 4 + x]) >> 24;

      if (rval> 0)
        std::cout <<" r= "<<r<< " a= "<<alpha<<"  rval="<<rval<<" aval="<<aval<<std::endl;
    }
  }

  // TODO(hrapp): cairo_image_surface_create_for_data does not take ownership of
  // the data until the surface is finalized. Once it is finalized though,
  // cairo_surface_write_to_png fails, complaining that the surface is already
  // finalized. This makes it pretty hard to pass back ownership of the image to
  // the caller.
  cairo::UniqueSurfacePtr surface(
      cairo_image_surface_create_for_data(
          reinterpret_cast<unsigned char*>(pixels.data()), CAIRO_FORMAT_ARGB32,
          mat.cols(), mat.rows(), stride),
      cairo_surface_destroy);
  CHECK_EQ(cairo_surface_status(surface.get()), CAIRO_STATUS_SUCCESS);
  CHECK_EQ(cairo_surface_write_to_png_stream(surface.get(), &CairoWriteCallback,
                                             file_writer),
           CAIRO_STATUS_SUCCESS);
  CHECK(file_writer->Close());
}

bool ContainedIn(const common::Time& time,
                 const std::vector<mapping::Timespan>& timespans) {
  for (const mapping::Timespan& timespan : timespans) {
    if (timespan.start <= time && time <= timespan.end) {
      return true;
    }
  }
  return false;
}

}  // namespace

XRayPointsProcessorMod::XRayPointsProcessorMod(
    const double voxel_size, const transform::Rigid3f& transform,
    const std::vector<mapping::Floor>& floors, const string& output_filename,
    FileWriterFactory file_writer_factory, PointsProcessor* const next)
    : next_(next),
      file_writer_factory_(file_writer_factory),
      floors_(floors),
      output_filename_(output_filename),
      transform_(transform),
      voxel_size_(voxel_size) {
  for (size_t i = 0; i < (floors_.empty() ? 1 : floors.size()); ++i) {
    aggregations_.emplace_back(
        Aggregation{mapping_3d::HybridGridBase<bool>(voxel_size), {}});
  }
}

std::unique_ptr<XRayPointsProcessorMod> XRayPointsProcessorMod::FromDictionary(
    const mapping::proto::Trajectory& trajectory,
    FileWriterFactory file_writer_factory,
    common::LuaParameterDictionary* const dictionary,
    PointsProcessor* const next) {
  std::vector<mapping::Floor> floors;
  if (dictionary->HasKey("separate_floors") &&
      dictionary->GetBool("separate_floors")) {
    floors = mapping::DetectFloors(trajectory);
  }

  return common::make_unique<XRayPointsProcessorMod>(
      dictionary->GetDouble("voxel_size"),
      transform::FromDictionary(dictionary->GetDictionary("transform").get())
          .cast<float>(),
      floors, dictionary->GetString("filename"), file_writer_factory, next);
}

void XRayPointsProcessorMod::WriteVoxels(const Aggregation& aggregation,
                                      FileWriter* const file_writer) {
  if (bounding_box_.isEmpty()) {
    LOG(WARNING) << "Not writing output: bounding box is empty.";
    return;
  }

  // Returns the (x, y) pixel of the given 'index'.
  const auto voxel_index_to_pixel = [this](const Eigen::Array3i& index) {
    // We flip the y axis, since matrices rows are counted from the top.
    return Eigen::Array2i(bounding_box_.max()[1] - index[1],
                          bounding_box_.max()[2] - index[2]);
  };

  // Hybrid grid uses X: forward, Y: left, Z: up.
  // For the screen we are using. X: right, Y: up
  const int xsize = bounding_box_.sizes()[1] + 1;
  const int ysize = bounding_box_.sizes()[2] + 1;
  PixelDataMatrix image = PixelDataMatrix(ysize, xsize);
  for (mapping_3d::HybridGridBase<bool>::Iterator it(aggregation.voxels);
       !it.Done(); it.Next()) {
    const Eigen::Array3i cell_index = it.GetCellIndex();
    const Eigen::Array2i pixel = voxel_index_to_pixel(cell_index);
    PixelData& pixel_data = image(pixel.y(), pixel.x());
    const auto& column_data = aggregation.column_data.at(
        std::make_pair(cell_index[1], cell_index[2]));
    pixel_data.mean_r = column_data.sum_r / column_data.count;
    pixel_data.mean_g = column_data.sum_g / column_data.count;
    pixel_data.mean_b = column_data.sum_b / column_data.count;
    ++pixel_data.num_occupied_cells_in_column;
  }
  WritePng(image, file_writer, voxel_size_);
}

void XRayPointsProcessorMod::Insert(const PointsBatch& batch,
                                 const transform::Rigid3f& transform,
                                 Aggregation* const aggregation) {
  constexpr Color kDefaultColor = {{0, 0, 0}};
  for (size_t i = 0; i < batch.points.size(); ++i) {
    const Eigen::Vector3f camera_point = transform * batch.points[i];
    const Eigen::Array3i cell_index =
        aggregation->voxels.GetCellIndex(camera_point);
    *aggregation->voxels.mutable_value(cell_index) = true;
    bounding_box_.extend(cell_index.matrix());
    ColumnData& column_data =
        aggregation->column_data[std::make_pair(cell_index[1], cell_index[2])];
    const auto& color =
        batch.colors.empty() ? kDefaultColor : batch.colors.at(i);
    column_data.sum_r += color[0];
    column_data.sum_g += color[1];
    column_data.sum_b += color[2];
    ++column_data.count;
  }
}

void XRayPointsProcessorMod::Process(std::unique_ptr<PointsBatch> batch) {
  if (floors_.empty()) {
    CHECK_EQ(aggregations_.size(), 1);
    Insert(*batch, transform_, &aggregations_[0]);
  } else {
    for (size_t i = 0; i < floors_.size(); ++i) {
      if (!ContainedIn(batch->time, floors_[i].timespans)) {
        continue;
      }
      Insert(*batch, transform_, &aggregations_[i]);
    }
  }
  next_->Process(std::move(batch));
}

PointsProcessor::FlushResult XRayPointsProcessorMod::Flush() {
  if (floors_.empty()) {
    CHECK_EQ(aggregations_.size(), 1);
    WriteVoxels(aggregations_[0],
                file_writer_factory_(output_filename_ + ".png").get());
  } else {
    for (size_t i = 0; i < floors_.size(); ++i) {
      WriteVoxels(
          aggregations_[i],
          file_writer_factory_(output_filename_ + std::to_string(i) + ".png")
              .get());
    }
  }

  switch (next_->Flush()) {
    case FlushResult::kRestartStream:
      LOG(FATAL) << "X-Ray generation must be configured to occur after any "
                    "stages that require multiple passes.";

    case FlushResult::kFinished:
      return FlushResult::kFinished;
  }
  LOG(FATAL);
}

}  // namespace io
}  // namespace cartographer