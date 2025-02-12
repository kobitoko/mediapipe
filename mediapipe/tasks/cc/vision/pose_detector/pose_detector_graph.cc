/* Copyright 2023 The MediaPipe Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <vector>

#include "absl/status/statusor.h"
#include "mediapipe/calculators/core/clip_vector_size_calculator.pb.h"
#include "mediapipe/calculators/tensor/image_to_tensor_calculator.pb.h"
#include "mediapipe/calculators/tensor/tensors_to_detections_calculator.pb.h"
#include "mediapipe/calculators/tflite/ssd_anchors_calculator.pb.h"
#include "mediapipe/calculators/util/detections_to_rects_calculator.pb.h"
#include "mediapipe/calculators/util/non_max_suppression_calculator.pb.h"
#include "mediapipe/calculators/util/rect_transformation_calculator.pb.h"
#include "mediapipe/framework/api2/builder.h"
#include "mediapipe/framework/calculator.pb.h"
#include "mediapipe/framework/formats/detection.pb.h"
#include "mediapipe/framework/formats/image.h"
#include "mediapipe/framework/formats/rect.pb.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/framework/subgraph.h"
#include "mediapipe/tasks/cc/components/processors/image_preprocessing_graph.h"
#include "mediapipe/tasks/cc/core/model_task_graph.h"
#include "mediapipe/tasks/cc/vision/pose_detector/proto/pose_detector_graph_options.pb.h"

namespace mediapipe {
namespace tasks {
namespace vision {
namespace pose_detector {

using ::mediapipe::NormalizedRect;
using ::mediapipe::Tensor;
using ::mediapipe::api2::Input;
using ::mediapipe::api2::Output;
using ::mediapipe::api2::builder::Graph;
using ::mediapipe::api2::builder::Source;
using ::mediapipe::tasks::vision::pose_detector::proto::
    PoseDetectorGraphOptions;

namespace {
constexpr char kImageTag[] = "IMAGE";
constexpr char kNormRectTag[] = "NORM_RECT";
constexpr char kTensorsTag[] = "TENSORS";
constexpr char kImageSizeTag[] = "IMAGE_SIZE";
constexpr char kAnchorsTag[] = "ANCHORS";
constexpr char kDetectionsTag[] = "DETECTIONS";
constexpr char kNormRectsTag[] = "NORM_RECTS";
constexpr char kPixelDetectionsTag[] = "PIXEL_DETECTIONS";
constexpr char kPoseRectsTag[] = "POSE_RECTS";
constexpr char kExpandedPoseRectsTag[] = "EXPANDED_POSE_RECTS";
constexpr char kMatrixTag[] = "MATRIX";
constexpr char kProjectionMatrixTag[] = "PROJECTION_MATRIX";

struct PoseDetectionOuts {
  Source<std::vector<Detection>> pose_detections;
  Source<std::vector<NormalizedRect>> pose_rects;
  Source<std::vector<NormalizedRect>> expanded_pose_rects;
  Source<Image> image;
};

// TODO: Configuration detection related calculators in pose
// detector with model metadata.
void ConfigureSsdAnchorsCalculator(
    mediapipe::SsdAnchorsCalculatorOptions* options) {
  // Dervied from
  // mediapipe/modules/pose_detection/pose_detection_gpu.pbtxt
  options->set_num_layers(5);
  options->set_min_scale(0.1484375);
  options->set_max_scale(0.75);
  options->set_input_size_height(224);
  options->set_input_size_width(224);
  options->set_anchor_offset_x(0.5);
  options->set_anchor_offset_y(0.5);
  options->add_strides(8);
  options->add_strides(16);
  options->add_strides(32);
  options->add_strides(32);
  options->add_strides(32);
  options->add_aspect_ratios(1.0);
  options->set_fixed_anchor_size(true);
}

// TODO: Configuration detection related calculators in pose
// detector with model metadata.
void ConfigureTensorsToDetectionsCalculator(
    const PoseDetectorGraphOptions& tasks_options,
    mediapipe::TensorsToDetectionsCalculatorOptions* options) {
  // Dervied from
  // mediapipe/modules/pose_detection/pose_detection_gpu.pbtxt
  options->set_num_classes(1);
  options->set_num_boxes(2254);
  options->set_num_coords(12);
  options->set_box_coord_offset(0);
  options->set_keypoint_coord_offset(4);
  options->set_num_keypoints(4);
  options->set_num_values_per_keypoint(2);
  options->set_sigmoid_score(true);
  options->set_score_clipping_thresh(100.0);
  options->set_reverse_output_order(true);
  options->set_min_score_thresh(tasks_options.min_detection_confidence());
  options->set_x_scale(224.0);
  options->set_y_scale(224.0);
  options->set_w_scale(224.0);
  options->set_h_scale(224.0);
}

void ConfigureNonMaxSuppressionCalculator(
    const PoseDetectorGraphOptions& tasks_options,
    mediapipe::NonMaxSuppressionCalculatorOptions* options) {
  options->set_min_suppression_threshold(
      tasks_options.min_suppression_threshold());
  options->set_overlap_type(
      mediapipe::NonMaxSuppressionCalculatorOptions::INTERSECTION_OVER_UNION);
  options->set_algorithm(
      mediapipe::NonMaxSuppressionCalculatorOptions::WEIGHTED);
}

// TODO: Configuration detection related calculators in pose
// detector with model metadata.
void ConfigureDetectionsToRectsCalculator(
    mediapipe::DetectionsToRectsCalculatorOptions* options) {
  options->set_rotation_vector_start_keypoint_index(0);
  options->set_rotation_vector_end_keypoint_index(2);
  options->set_rotation_vector_target_angle(90);
  options->set_output_zero_rect_for_empty_detections(true);
}

// TODO: Configuration detection related calculators in pose
// detector with model metadata.
void ConfigureRectTransformationCalculator(
    mediapipe::RectTransformationCalculatorOptions* options) {
  options->set_scale_x(2.6);
  options->set_scale_y(2.6);
  options->set_shift_y(-0.5);
  options->set_square_long(true);
}

}  // namespace

// A "mediapipe.tasks.vision.pose_detector.PoseDetectorGraph" performs pose
// detection.
//
// Inputs:
//   IMAGE - Image
//     Image to perform detection on.
//   NORM_RECT - NormalizedRect @Optional
//     Describes image rotation and region of image to perform detection on. If
//     not provided, whole image is used for pose detection.
//
// Outputs:
//   DETECTIONS - std::vector<Detection>
//     Detected pose with maximum `num_poses` specified in options.
//   POSE_RECTS - std::vector<NormalizedRect>
//     Detected pose bounding boxes in normalized coordinates.
//   EXPANDED_POSE_RECTS - std::vector<NormalizedRect>
//     Expanded pose bounding boxes in normalized coordinates so that bounding
//     boxes likely contain the whole pose. This is usually used as RoI for pose
//     landmarks detection to run on.
//   IMAGE - Image
//     The input image that the pose detector runs on and has the pixel data
//     stored on the target storage (CPU vs GPU).
// All returned coordinates are in the unrotated and uncropped input image
// coordinates system.
//
// Example:
// node {
//   calculator: "mediapipe.tasks.vision.pose_detector.PoseDetectorGraph"
//   input_stream: "IMAGE:image"
//   input_stream: "NORM_RECT:norm_rect"
//   output_stream: "DETECTIONS:palm_detections"
//   output_stream: "POSE_RECTS:pose_rects"
//   output_stream: "EXPANDED_POSE_RECTS:expanded_pose_rects"
//   output_stream: "IMAGE:image_out"
//   options {
//     [mediapipe.tasks.vision.pose_detector.proto.PoseDetectorGraphOptions.ext]
//     {
//       base_options {
//          model_asset {
//            file_name: "pose_detection.tflite"
//          }
//       }
//       min_detection_confidence: 0.5
//       num_poses: 2
//     }
//   }
// }
class PoseDetectorGraph : public core::ModelTaskGraph {
 public:
  absl::StatusOr<CalculatorGraphConfig> GetConfig(
      SubgraphContext* sc) override {
    ASSIGN_OR_RETURN(const auto* model_resources,
                     CreateModelResources<PoseDetectorGraphOptions>(sc));
    Graph graph;
    ASSIGN_OR_RETURN(auto outs,
                     BuildPoseDetectionSubgraph(
                         sc->Options<PoseDetectorGraphOptions>(),
                         *model_resources, graph[Input<Image>(kImageTag)],
                         graph[Input<NormalizedRect>(kNormRectTag)], graph));

    outs.pose_detections >>
        graph.Out(kDetectionsTag).Cast<std::vector<Detection>>();
    outs.pose_rects >>
        graph.Out(kPoseRectsTag).Cast<std::vector<NormalizedRect>>();
    outs.expanded_pose_rects >>
        graph.Out(kExpandedPoseRectsTag).Cast<std::vector<NormalizedRect>>();
    outs.image >> graph.Out(kImageTag).Cast<Image>();

    return graph.GetConfig();
  }

 private:
  absl::StatusOr<PoseDetectionOuts> BuildPoseDetectionSubgraph(
      const PoseDetectorGraphOptions& subgraph_options,
      const core::ModelResources& model_resources, Source<Image> image_in,
      Source<NormalizedRect> norm_rect_in, Graph& graph) {
    // Image preprocessing subgraph to convert image to tensor for the tflite
    // model.
    auto& preprocessing = graph.AddNode(
        "mediapipe.tasks.components.processors.ImagePreprocessingGraph");
    bool use_gpu =
        components::processors::DetermineImagePreprocessingGpuBackend(
            subgraph_options.base_options().acceleration());
    MP_RETURN_IF_ERROR(components::processors::ConfigureImagePreprocessingGraph(
        model_resources, use_gpu,
        &preprocessing.GetOptions<
            components::processors::proto::ImagePreprocessingGraphOptions>()));
    auto& image_to_tensor_options =
        *preprocessing
             .GetOptions<components::processors::proto::
                             ImagePreprocessingGraphOptions>()
             .mutable_image_to_tensor_options();
    image_to_tensor_options.set_keep_aspect_ratio(true);
    image_to_tensor_options.set_border_mode(
        mediapipe::ImageToTensorCalculatorOptions::BORDER_ZERO);
    image_in >> preprocessing.In(kImageTag);
    norm_rect_in >> preprocessing.In(kNormRectTag);
    auto preprocessed_tensors = preprocessing.Out(kTensorsTag);
    auto matrix = preprocessing.Out(kMatrixTag);
    auto image_size = preprocessing.Out(kImageSizeTag);

    // Pose detection model inferece.
    auto& inference = AddInference(
        model_resources, subgraph_options.base_options().acceleration(), graph);
    preprocessed_tensors >> inference.In(kTensorsTag);
    auto model_output_tensors =
        inference.Out(kTensorsTag).Cast<std::vector<Tensor>>();

    // Generates a single side packet containing a vector of SSD anchors.
    auto& ssd_anchor = graph.AddNode("SsdAnchorsCalculator");
    ConfigureSsdAnchorsCalculator(
        &ssd_anchor.GetOptions<mediapipe::SsdAnchorsCalculatorOptions>());
    auto anchors = ssd_anchor.SideOut("");

    // Converts output tensors to Detections.
    auto& tensors_to_detections =
        graph.AddNode("TensorsToDetectionsCalculator");
    ConfigureTensorsToDetectionsCalculator(
        subgraph_options,
        &tensors_to_detections
             .GetOptions<mediapipe::TensorsToDetectionsCalculatorOptions>());
    model_output_tensors >> tensors_to_detections.In(kTensorsTag);
    anchors >> tensors_to_detections.SideIn(kAnchorsTag);
    auto detections = tensors_to_detections.Out(kDetectionsTag);

    // Non maximum suppression removes redundant face detections.
    auto& non_maximum_suppression =
        graph.AddNode("NonMaxSuppressionCalculator");
    ConfigureNonMaxSuppressionCalculator(
        subgraph_options,
        &non_maximum_suppression
             .GetOptions<mediapipe::NonMaxSuppressionCalculatorOptions>());
    detections >> non_maximum_suppression.In("");
    auto nms_detections = non_maximum_suppression.Out("");

    // Projects detections back into the input image coordinates system.
    auto& detection_projection = graph.AddNode("DetectionProjectionCalculator");
    nms_detections >> detection_projection.In(kDetectionsTag);
    matrix >> detection_projection.In(kProjectionMatrixTag);
    Source<std::vector<Detection>> pose_detections =
        detection_projection.Out(kDetectionsTag).Cast<std::vector<Detection>>();

    if (subgraph_options.has_num_poses()) {
      // Clip face detections to maximum number of poses.
      auto& clip_detection_vector_size =
          graph.AddNode("ClipDetectionVectorSizeCalculator");
      clip_detection_vector_size
          .GetOptions<mediapipe::ClipVectorSizeCalculatorOptions>()
          .set_max_vec_size(subgraph_options.num_poses());
      pose_detections >> clip_detection_vector_size.In("");
      pose_detections =
          clip_detection_vector_size.Out("").Cast<std::vector<Detection>>();
    }

    // Converts results of pose detection into a rectangle (normalized by image
    // size) that encloses the face and is rotated such that the line connecting
    // left eye and right eye is aligned with the X-axis of the rectangle.
    auto& detections_to_rects = graph.AddNode("DetectionsToRectsCalculator");
    ConfigureDetectionsToRectsCalculator(
        &detections_to_rects
             .GetOptions<mediapipe::DetectionsToRectsCalculatorOptions>());
    image_size >> detections_to_rects.In(kImageSizeTag);
    pose_detections >> detections_to_rects.In(kDetectionsTag);
    auto pose_rects = detections_to_rects.Out(kNormRectsTag)
                          .Cast<std::vector<NormalizedRect>>();

    // Expands and shifts the rectangle that contains the pose so that it's
    // likely to cover the entire pose.
    auto& rect_transformation = graph.AddNode("RectTransformationCalculator");
    ConfigureRectTransformationCalculator(
        &rect_transformation
             .GetOptions<mediapipe::RectTransformationCalculatorOptions>());
    pose_rects >> rect_transformation.In(kNormRectsTag);
    image_size >> rect_transformation.In(kImageSizeTag);
    auto expanded_pose_rects =
        rect_transformation.Out("").Cast<std::vector<NormalizedRect>>();

    // Calculator to convert relative detection bounding boxes to pixel
    // detection bounding boxes.
    auto& detection_transformation =
        graph.AddNode("DetectionTransformationCalculator");
    detection_projection.Out(kDetectionsTag) >>
        detection_transformation.In(kDetectionsTag);
    preprocessing.Out(kImageSizeTag) >>
        detection_transformation.In(kImageSizeTag);
    auto pose_pixel_detections =
        detection_transformation.Out(kPixelDetectionsTag)
            .Cast<std::vector<Detection>>();

    return PoseDetectionOuts{
        /* pose_detections= */ pose_pixel_detections,
        /* pose_rects= */ pose_rects,
        /* expanded_pose_rects= */ expanded_pose_rects,
        /* image= */ preprocessing.Out(kImageTag).Cast<Image>()};
  }
};

REGISTER_MEDIAPIPE_GRAPH(
    ::mediapipe::tasks::vision::pose_detector::PoseDetectorGraph);

}  // namespace pose_detector
}  // namespace vision
}  // namespace tasks
}  // namespace mediapipe
