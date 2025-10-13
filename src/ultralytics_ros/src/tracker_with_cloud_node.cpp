/**
 * ultralytics_ros
 * Copyright (C) 2023-2024  Alpaca-zip
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tracker_with_cloud_node/tracker_with_cloud_node.h"
#include <cmath>

TrackerWithCloudNode::TrackerWithCloudNode() : rclcpp::Node("tracker_with_cloud_node")
{
  this->declare_parameter<std::string>("camera_info_topic", "camera_info");
  this->declare_parameter<std::string>("lidar_topic", "points_raw");
  this->declare_parameter<std::string>("yolo_result_topic", "yolo_result");
  this->declare_parameter<std::string>("yolo_3d_result_topic", "yolo_3d_result");
  this->declare_parameter<float>("cluster_tolerance", 0.5);
  this->declare_parameter<float>("voxel_leaf_size", 0.5);
  this->declare_parameter<int>("min_cluster_size", 100);
  this->declare_parameter<int>("max_cluster_size", 25000);
  // === [ADD] ERP/Pinhole 스위치 파라미터 선언 ===
  this->declare_parameter<std::string>("projection_model", "pinhole"); // "pinhole" | "erp"
  this->declare_parameter<double>("erp_yaw_offset_deg", 0.0);
  this->declare_parameter<bool>("erp_flip_x", false);
  this->declare_parameter<bool>("erp_flip_y", false);

  this->get_parameter("camera_info_topic", camera_info_topic_);
  this->get_parameter("lidar_topic", lidar_topic_);
  this->get_parameter("yolo_result_topic", yolo_result_topic_);
   // === [ADD] ERP/Pinhole 스위치 파라미터 로드 ===
  this->get_parameter("projection_model", projection_model_);
  this->get_parameter("erp_yaw_offset_deg", erp_yaw_offset_deg_);
  this->get_parameter("erp_flip_x", erp_flip_x_);
  this->get_parameter("erp_flip_y", erp_flip_y_);
  RCLCPP_INFO(this->get_logger(), "projection_model=%s", projection_model_.c_str());
  camera_info_sub_.subscribe(this, camera_info_topic_);
  lidar_sub_.subscribe(this, lidar_topic_);
  yolo_result_sub_.subscribe(this, yolo_result_topic_);
  sync_ = std::make_shared<message_filters::Synchronizer<ApproximateSyncPolicy>>(20);
  sync_->connectInput(camera_info_sub_, lidar_sub_, yolo_result_sub_);
  sync_->registerCallback(std::bind(&TrackerWithCloudNode::syncCallback, this, std::placeholders::_1,
                                    std::placeholders::_2, std::placeholders::_3));

  this->get_parameter("yolo_3d_result_topic", yolo_3d_result_topic_);
  detection3d_pub_ = this->create_publisher<vision_msgs::msg::Detection3DArray>(yolo_3d_result_topic_, 1);
  detection_cloud_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("detection_cloud", 1);
  marker_pub_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("detection_marker", 1);

  last_call_time_ = this->now();
  tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
  tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);
}

void TrackerWithCloudNode::syncCallback(
    const sensor_msgs::msg::CameraInfo::ConstSharedPtr& camera_info_msg,
    const sensor_msgs::msg::PointCloud2::ConstSharedPtr& cloud_msg,
    const ultralytics_ros::msg::YoloResult::ConstSharedPtr& yolo_result_msg)
{
  rclcpp::Time current_call_time = this->now();
  rclcpp::Duration callback_interval = current_call_time - last_call_time_;
  last_call_time_ = current_call_time;

  // 1) 다운샘플
  pcl::PointCloud<pcl::PointXYZ>::Ptr downsampled_cloud(new pcl::PointCloud<pcl::PointXYZ>());
  downsampled_cloud = downsampleCloudMsg(cloud_msg);

  // 2) 이미지 크기 저장(ERP용)
  last_img_w_ = static_cast<int>(camera_info_msg->width);
  last_img_h_ = static_cast<int>(camera_info_msg->height);

  // 3) 투영 모델에 따라 카메라 프레임 결정
  if (projection_model_ == "pinhole") {
    cam_model_.fromCameraInfo(camera_info_msg);   // 기존 핀홀 경로 유지
    camera_frame_ = cam_model_.tfFrame();         // *_optical
  } else { // "erp"
    // ERP는 K/D 안 쓰고 frame_id만 사용
    camera_frame_ = camera_info_msg->header.frame_id; // 예: theta_camera_optical
  }

  // 4) LiDAR → 카메라 프레임으로 변환
  pcl::PointCloud<pcl::PointXYZ>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZ>());
  transformed_cloud = cloud2TransformedCloud(
      downsampled_cloud,
      cloud_msg->header.frame_id,
      camera_frame_,                           // ★ cam_model_.tfFrame() 대신 camera_frame_
      cloud_msg->header.stamp);

  // 5) 3D 생성
  vision_msgs::msg::Detection3DArray detection3d_array_msg;
  sensor_msgs::msg::PointCloud2 detection_cloud_msg;
  projectCloud(transformed_cloud, yolo_result_msg, cloud_msg->header,
               detection3d_array_msg, detection_cloud_msg);

  // 6) 마커 및 퍼블리시
  visualization_msgs::msg::MarkerArray marker_array_msg =
      createMarkerArray(detection3d_array_msg, callback_interval.seconds());

  detection3d_pub_->publish(detection3d_array_msg);
  detection_cloud_pub_->publish(detection_cloud_msg);
  marker_pub_->publish(marker_array_msg);
}


void TrackerWithCloudNode::transformPointCloud(const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud_in,
                                               pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud_out,
                                               const Eigen::Affine3f& transform)
{
  int cloud_size = cloud_in->size();
  cloud_out->resize(cloud_size);

  for (int i = 0; i < cloud_size; i++)
  {
    const auto& point = cloud_in->points[i];
    cloud_out->points[i].x =
        transform(0, 0) * point.x + transform(0, 1) * point.y + transform(0, 2) * point.z + transform(0, 3);
    cloud_out->points[i].y =
        transform(1, 0) * point.x + transform(1, 1) * point.y + transform(1, 2) * point.z + transform(1, 3);
    cloud_out->points[i].z =
        transform(2, 0) * point.x + transform(2, 1) * point.y + transform(2, 2) * point.z + transform(2, 3);
  }
}

bool TrackerWithCloudNode::projectToPixel(const cv::Point3d& P,
                                          double& u, double& v,
                                          int img_w, int img_h) const
{
  if (projection_model_ == "pinhole") {
    cv::Point2d uv = cam_model_.project3dToPixel(P);
    u = uv.x; v = uv.y;
    return std::isfinite(u) && std::isfinite(v);
  }

  // === ERP ===
  double x=P.x, y=P.y, z=P.z;
  double yaw = erp_yaw_offset_deg_ * M_PI / 180.0;

  // 수평(yaw) 회전(Y축)
  double xr =  std::cos(yaw)*x + std::sin(yaw)*z;
  double zr = -std::sin(yaw)*x + std::cos(yaw)*z;

  // 경도/위도
  double lon = std::atan2(xr, zr);                       // [-pi, pi]
  double lat = std::atan2(y, std::sqrt(xr*xr + zr*zr));  // [-pi/2, pi/2]

  // ERP 픽셀
  u = (lon + M_PI) / (2.0 * M_PI) * img_w;
  v = (M_PI/2.0 - lat) / M_PI * img_h;

  if (erp_flip_x_) u = img_w - u;
  if (erp_flip_y_) v = img_h - v;

  return (u >= 0 && u < img_w && v >= 0 && v < img_h);
}



void TrackerWithCloudNode::projectCloud(const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud,
                                        const ultralytics_ros::msg::YoloResult::ConstSharedPtr& yolo_result_msg,
                                        const std_msgs::msg::Header& header,
                                        vision_msgs::msg::Detection3DArray& detection3d_array_msg,
                                        sensor_msgs::msg::PointCloud2& combine_detection_cloud_msg)
{
  pcl::PointCloud<pcl::PointXYZ>::Ptr combine_detection_cloud(new pcl::PointCloud<pcl::PointXYZ>());
  detection3d_array_msg.header = header;
  detection3d_array_msg.header.stamp = yolo_result_msg->header.stamp;

  for (size_t i = 0; i < yolo_result_msg->detections.detections.size(); i++)
  {
    pcl::PointCloud<pcl::PointXYZ>::Ptr detection_cloud_raw(new pcl::PointCloud<pcl::PointXYZ>());

    if (yolo_result_msg->masks.empty())
    {
      processPointsWithBbox(cloud, yolo_result_msg->detections.detections[i], detection_cloud_raw);
    }
    else
    {
      processPointsWithMask(cloud, yolo_result_msg->masks[i], detection_cloud_raw);
    }

    if (!detection_cloud_raw->points.empty())
    {
      pcl::PointCloud<pcl::PointXYZ>::Ptr detection_cloud(new pcl::PointCloud<pcl::PointXYZ>());
      detection_cloud = cloud2TransformedCloud(detection_cloud_raw, camera_frame_, header.frame_id, header.stamp);

      pcl::PointCloud<pcl::PointXYZ>::Ptr closest_detection_cloud(new pcl::PointCloud<pcl::PointXYZ>());
      closest_detection_cloud = euclideanClusterExtraction(detection_cloud);
      *combine_detection_cloud += *closest_detection_cloud;

      createBoundingBox(detection3d_array_msg, closest_detection_cloud,
                        yolo_result_msg->detections.detections[i].results);
    }
  }

  pcl::toROSMsg(*combine_detection_cloud, combine_detection_cloud_msg);
  combine_detection_cloud_msg.header = header;
}
// === 추가: ERP용 bbox 포함 판정 (시접 래핑 처리) ===
namespace {
inline bool in_bbox_erp_wrapped(double u, double v,
                                const vision_msgs::msg::Detection2D& det,
                                int img_w, int img_h)
{
  const double cx = det.bbox.center.position.x;
  const double cy = det.bbox.center.position.y;
  const double sx = det.bbox.size_x;
  const double sy = det.bbox.size_y;

  double x0 = cx - sx * 0.5;
  double x1 = cx + sx * 0.5;
  double y0 = cy - sy * 0.5;
  double y1 = cy + sy * 0.5;

  // 수직은 평범하게
  if (v < y0 || v > y1) return false;

  // 수평: 래핑 없는 일반 케이스
  if (x0 >= 0.0 && x1 < img_w) {
    return (u >= x0 && u <= x1);
  }

  // 시접 래핑 케이스: [x0, x1]가 화면 폭을 넘어감
  // 범위를 [0, 2W)로 옮겨서 두 구간으로 분리
  while (x0 < 0.0) { x0 += img_w; x1 += img_w; }
  // 이제 x0 in [0, +inf), x1 >= x0
  // 실제 픽셀 u는 [0, W)라서, [x0, W) ∪ [0, x1 - W) 중 하나에 들어가면 OK
  double x1_mod = std::fmod(x1, (double)img_w); // (x1 - W)
  return (u >= x0 && u < img_w) || (u >= 0.0 && u <= x1_mod);
}
} // namespace

void TrackerWithCloudNode::processPointsWithBbox(
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud,
    const vision_msgs::msg::Detection2D& detection2d_msg,
    pcl::PointCloud<pcl::PointXYZ>::Ptr& detection_cloud_raw)
{
  const bool is_pinhole = (projection_model_ == "pinhole");

  for (const auto& point : cloud->points)
  {
    cv::Point3d P(point.x, point.y, point.z);
    double u, v;
    if (!projectToPixel(P, u, v, last_img_w_, last_img_h_)) continue;

    // 핀홀: 앞쪽(z>0)만 사용, 일반 bbox 체크
    if (is_pinhole) {
      if (P.z <= 0.0) continue;
      if (u >= detection2d_msg.bbox.center.position.x - detection2d_msg.bbox.size_x * 0.5 &&
          u <= detection2d_msg.bbox.center.position.x + detection2d_msg.bbox.size_x * 0.5 &&
          v >= detection2d_msg.bbox.center.position.y - detection2d_msg.bbox.size_y * 0.5 &&
          v <= detection2d_msg.bbox.center.position.y + detection2d_msg.bbox.size_y * 0.5)
      {
        detection_cloud_raw->points.push_back(point);
      }
    }
    // ERP: z>0 컷 제거, 시접 래핑 포함 판정 사용
    else {
      if (in_bbox_erp_wrapped(u, v, detection2d_msg, last_img_w_, last_img_h_)) {
        detection_cloud_raw->points.push_back(point);
      }
    }
  }
}

void TrackerWithCloudNode::processPointsWithMask(
    const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud,
    const sensor_msgs::msg::Image& mask_image_msg,
    pcl::PointCloud<pcl::PointXYZ>::Ptr& detection_cloud_raw)
{
  cv_bridge::CvImagePtr cv_ptr;
  try {
    cv_ptr = cv_bridge::toCvCopy(mask_image_msg, sensor_msgs::image_encodings::MONO8);
  } catch (cv_bridge::Exception& e) {
    RCLCPP_ERROR(this->get_logger(), "%s", e.what());
    return;
  }

  const bool is_pinhole = (projection_model_ == "pinhole");
  const int W = cv_ptr->image.cols;
  const int H = cv_ptr->image.rows;

  for (const auto& point : cloud->points)
  {
    cv::Point3d P(point.x, point.y, point.z);
    double u, v;
    if (!projectToPixel(P, u, v, W, H)) continue;

    // 핀홀은 앞쪽만
    if (is_pinhole && P.z <= 0.0) continue;

    if (u >= 0.0 && u < W && v >= 0.0 && v < H)
    {
      const int uu = static_cast<int>(u);
      const int vv = static_cast<int>(v);
      if (cv_ptr->image.at<uchar>(cv::Point(uu, vv)) > 0) {
        detection_cloud_raw->points.push_back(point);
      }
    }
  }
}

void TrackerWithCloudNode::createBoundingBox(
    vision_msgs::msg::Detection3DArray& detection3d_array_msg, const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud,
    const std::vector<vision_msgs::msg::ObjectHypothesisWithPose>& detections_results_msg)
{
  Eigen::Vector4f centroid;
  pcl::compute3DCentroid(*cloud, centroid);
  double theta = -atan2(centroid[1], sqrt(pow(centroid[0], 2) + pow(centroid[2], 2)));
  Eigen::Affine3f transform = Eigen::Affine3f::Identity();
  transform.rotate(Eigen::AngleAxisf(theta, Eigen::Vector3f::UnitZ()));

  pcl::PointCloud<pcl::PointXYZ>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZ>());
  transformPointCloud(cloud, transformed_cloud, transform);

  pcl::PointXYZ min_pt, max_pt;
  pcl::getMinMax3D(*transformed_cloud, min_pt, max_pt);
  Eigen::Vector4f transformed_bbox_center =
      Eigen::Vector4f((min_pt.x + max_pt.x) / 2, (min_pt.y + max_pt.y) / 2, (min_pt.z + max_pt.z) / 2, 1);
  Eigen::Vector4f bbox_center = transform.inverse() * transformed_bbox_center;
  Eigen::Quaternionf q(transform.inverse().rotation());

  vision_msgs::msg::Detection3D detection3d_msg;
  detection3d_msg.bbox.center.position.x = bbox_center[0];
  detection3d_msg.bbox.center.position.y = bbox_center[1];
  detection3d_msg.bbox.center.position.z = bbox_center[2];
  detection3d_msg.bbox.center.orientation.x = q.x();
  detection3d_msg.bbox.center.orientation.y = q.y();
  detection3d_msg.bbox.center.orientation.z = q.z();
  detection3d_msg.bbox.center.orientation.w = q.w();
  detection3d_msg.bbox.size.x = max_pt.x - min_pt.x;
  detection3d_msg.bbox.size.y = max_pt.y - min_pt.y;
  detection3d_msg.bbox.size.z = max_pt.z - min_pt.z;
  detection3d_msg.results = detections_results_msg;
  detection3d_array_msg.detections.push_back(detection3d_msg);
}

pcl::PointCloud<pcl::PointXYZ>::Ptr
TrackerWithCloudNode::downsampleCloudMsg(const sensor_msgs::msg::PointCloud2::ConstSharedPtr& cloud_msg)
{
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>());
  pcl::fromROSMsg(*cloud_msg, *cloud);

  this->get_parameter("voxel_leaf_size", voxel_leaf_size_);
  pcl::PointCloud<pcl::PointXYZ>::Ptr downsampled_cloud(new pcl::PointCloud<pcl::PointXYZ>());
  pcl::VoxelGrid<pcl::PointXYZ>::Ptr voxel_grid(new pcl::VoxelGrid<pcl::PointXYZ>());
  voxel_grid->setInputCloud(cloud);
  voxel_grid->setLeafSize(voxel_leaf_size_, voxel_leaf_size_, voxel_leaf_size_);
  voxel_grid->filter(*downsampled_cloud);

  return downsampled_cloud;
}

pcl::PointCloud<pcl::PointXYZ>::Ptr
TrackerWithCloudNode::cloud2TransformedCloud(const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud,
                                             const std::string& source_frame, const std::string& target_frame,
                                             const rclcpp::Time& stamp)
{
  try
  {
    geometry_msgs::msg::TransformStamped tf_stamped = tf_buffer_->lookupTransform(target_frame, source_frame, stamp);
    Eigen::Affine3f eigen_transform = tf2::transformToEigen(tf_stamped.transform).cast<float>();
    pcl::PointCloud<pcl::PointXYZ>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZ>());
    transformPointCloud(cloud, transformed_cloud, eigen_transform);

    return transformed_cloud;
  }
  catch (tf2::TransformException& e)
  {
    RCLCPP_WARN(this->get_logger(), "%s", e.what());
    return cloud;
  }
}

pcl::PointCloud<pcl::PointXYZ>::Ptr
TrackerWithCloudNode::euclideanClusterExtraction(const pcl::PointCloud<pcl::PointXYZ>::Ptr& cloud)
{
  this->get_parameter("cluster_tolerance", cluster_tolerance_);
  this->get_parameter("min_cluster_size", min_cluster_size_);
  this->get_parameter("max_cluster_size", max_cluster_size_);
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>());
  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
  ec.setClusterTolerance(cluster_tolerance_);
  ec.setMinClusterSize(min_cluster_size_);
  ec.setMaxClusterSize(max_cluster_size_);
  ec.setSearchMethod(tree);
  ec.setInputCloud(cloud);
  ec.extract(cluster_indices);

  float min_distance = std::numeric_limits<float>::max();
  pcl::PointCloud<pcl::PointXYZ>::Ptr closest_cluster(new pcl::PointCloud<pcl::PointXYZ>());

  for (const auto& cluster : cluster_indices)
  {
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster(new pcl::PointCloud<pcl::PointXYZ>());
    for (const auto& indice : cluster.indices)
    {
      cloud_cluster->push_back((*cloud)[indice]);
    }

    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*cloud_cluster, centroid);
    float distance = centroid.norm();

    if (distance < min_distance)
    {
      min_distance = distance;
      *closest_cluster = *cloud_cluster;
    }
  }

  return closest_cluster;
}

visualization_msgs::msg::MarkerArray
TrackerWithCloudNode::createMarkerArray(const vision_msgs::msg::Detection3DArray& detection3d_array_msg,
                                        const double& duration)
{
  visualization_msgs::msg::MarkerArray marker_array_msg;

  for (size_t i = 0; i < detection3d_array_msg.detections.size(); i++)
  {
    if (std::isfinite(detection3d_array_msg.detections[i].bbox.size.x) &&
        std::isfinite(detection3d_array_msg.detections[i].bbox.size.y) &&
        std::isfinite(detection3d_array_msg.detections[i].bbox.size.z))
    {
      visualization_msgs::msg::Marker marker_msg;
      marker_msg.header = detection3d_array_msg.header;
      marker_msg.ns = "detection";
      marker_msg.id = i;
      marker_msg.type = visualization_msgs::msg::Marker::CUBE;
      marker_msg.action = visualization_msgs::msg::Marker::ADD;
      marker_msg.pose = detection3d_array_msg.detections[i].bbox.center;
      marker_msg.scale.x = detection3d_array_msg.detections[i].bbox.size.x;
      marker_msg.scale.y = detection3d_array_msg.detections[i].bbox.size.y;
      marker_msg.scale.z = detection3d_array_msg.detections[i].bbox.size.z;
      marker_msg.color.r = 0.0;
      marker_msg.color.g = 1.0;
      marker_msg.color.b = 0.0;
      marker_msg.color.a = 0.5;
      marker_msg.lifetime = rclcpp::Duration(std::chrono::duration<double>(duration));
      marker_array_msg.markers.push_back(marker_msg);
    }
  }

  return marker_array_msg;
}

int main(int argc, char* argv[])
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<TrackerWithCloudNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
