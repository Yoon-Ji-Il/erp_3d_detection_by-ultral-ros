#!/usr/bin/env python3
import rclpy
from rclpy.node import Node
from sensor_msgs.msg import Image, CameraInfo
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, HistoryPolicy

class CISync(Node):
    def __init__(self):
        super().__init__('ci_sync')

        # 새 bag 기본값
        self.declare_parameter('image_topic', '/ricoh/image_raw')
        self.declare_parameter('camera_info_topic', '/ricoh/camera_info')
        self.declare_parameter('frame_id', 'ricoh_camera_optical')
        self.declare_parameter('width', 1920)
        self.declare_parameter('height', 960)

        img_topic = self.get_parameter('image_topic').value
        self.ci_topic = self.get_parameter('camera_info_topic').value
        self.frame = self.get_parameter('frame_id').value
        self.w = int(self.get_parameter('width').value)
        self.h = int(self.get_parameter('height').value)

        # 이미지 구독: SensorData QoS (BEST_EFFORT)
        sensor_qos = QoSProfile(
            depth=5,
            reliability=ReliabilityPolicy.BEST_EFFORT,
            durability=DurabilityPolicy.VOLATILE,
            history=HistoryPolicy.KEEP_LAST
        )
        self.sub = self.create_subscription(Image, img_topic, self.cb, sensor_qos)

        # ★ CameraInfo 퍼블리시: RELIABLE (구독자와 호환)
        ci_pub_qos = QoSProfile(
            depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE,
            history=HistoryPolicy.KEEP_LAST
        )
        self.pub = self.create_publisher(CameraInfo, self.ci_topic, ci_pub_qos)

        self.get_logger().info(
            f"Syncing CameraInfo on {self.ci_topic} from {img_topic} "
            f"({self.w}x{self.h}, frame={self.frame})"
        )

    def cb(self, img: Image):
        ci = CameraInfo()
        ci.header = img.header
        ci.header.frame_id = self.frame
        ci.width = self.w
        ci.height = self.h
        self.pub.publish(ci)

def main():
    rclpy.init()
    n = CISync()
    try:
        rclpy.spin(n)
    except KeyboardInterrupt:
        pass
    finally:
        rclpy.shutdown()

if __name__ == '__main__':
    main()

