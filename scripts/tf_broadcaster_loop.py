#!/usr/bin/env python3
import math, rclpy
from rclpy.node import Node
from geometry_msgs.msg import TransformStamped
from tf2_ros import TransformBroadcaster

class TFB(Node):
    def __init__(self):
        super().__init__('erp_livox_tf_broadcaster')
        # ★ 기본 프레임 새 bag에 맞춤
        self.declare_parameter('parent', 'ricoh_camera_optical')  # 카메라(부모)
        self.declare_parameter('child',  'livox_frame')           # LiDAR(자식)
        self.declare_parameter('xyz', [0.0, 0.0, 0.0])
        self.declare_parameter('rpy', [0.0, 0.0, 0.0])
        # ★ sim time 사용 기본값
      

        self.br = TransformBroadcaster(self)
        self.timer = self.create_timer(0.1, self.tick)  # 10 Hz

    def tick(self):
        t = TransformStamped()
        t.header.stamp = self.get_clock().now().to_msg()
        t.header.frame_id = self.get_parameter('parent').value
        t.child_frame_id  = self.get_parameter('child').value
        x,y,z = self.get_parameter('xyz').value
        r,p,yw = self.get_parameter('rpy').value
        cy, sy = math.cos(yw*0.5), math.sin(yw*0.5)
        cp, sp = math.cos(p*0.5),  math.sin(p*0.5)
        cr, sr = math.cos(r*0.5),  math.sin(r*0.5)
        t.transform.translation.x = float(x)
        t.transform.translation.y = float(y)
        t.transform.translation.z = float(z)
        t.transform.rotation.w = cr*cp*cy + sr*sp*sy
        t.transform.rotation.x = sr*cp*cy - cr*sp*sy
        t.transform.rotation.y = cr*sp*cy + sr*cp*sy
        t.transform.rotation.z = cr*cp*sy - sr*cp*cy
        self.br.sendTransform(t)

def main():
    rclpy.init()
    n = TFB()
    try:
        rclpy.spin(n)
    except KeyboardInterrupt:
        pass
    finally:
        rclpy.shutdown()

if __name__ == '__main__':
    main()

