cmake_minimum_required(VERSION 3.8)
project(ultralytics_ros)
set(CMAKE_CXX_STANDARD 17)

# -------------------------------
# Dependencies
# -------------------------------
find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclpy REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(vision_msgs REQUIRED)
find_package(visualization_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(image_geometry REQUIRED)
find_package(message_filters REQUIRED)
find_package(pcl_conversions REQUIRED)
find_package(tf2_eigen REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(OpenCV REQUIRED)
find_package(PCL REQUIRED COMPONENTS common io filters segmentation search)

# -------------------------------
# Custom Message Build
# -------------------------------
rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/YoloResult.msg"
  DEPENDENCIES builtin_interfaces std_msgs sensor_msgs vision_msgs
)
ament_export_dependencies(rosidl_default_runtime)

# -------------------------------
# Includes
# -------------------------------
include_directories(
  include
  ${PCL_INCLUDE_DIRS}
)
add_definitions(${PCL_DEFINITIONS})

# -------------------------------
# Build Node
# -------------------------------
add_executable(tracker_with_cloud_node src/tracker_with_cloud_node.cpp)

ament_target_dependencies(tracker_with_cloud_node
  rclcpp
  geometry_msgs
  sensor_msgs
  std_msgs
  vision_msgs
  visualization_msgs
  cv_bridge
  image_geometry
  message_filters
  pcl_conversions
  tf2_eigen
  tf2_ros
  OpenCV
  PCL
)

# rosidl generated cpp headers
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(tracker_with_cloud_node "${cpp_typesupport_target}" ${PCL_LIBRARIES})

# -------------------------------
# Install
# -------------------------------
install(TARGETS tracker_with_cloud_node
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(PROGRAMS
  script/tracker_node.py
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY
  launch
  rviz2
  DESTINATION share/${PROJECT_NAME}
)

ament_package()

