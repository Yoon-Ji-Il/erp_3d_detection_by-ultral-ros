#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# ultralytics_ros - Tracker Node (with external OC-SORT support)

import os
import yaml
import numpy as np
import rclpy
import cv_bridge

from ament_index_python.packages import get_package_share_directory
from rclpy.node import Node
from sensor_msgs.msg import Image
from vision_msgs.msg import Detection2D, Detection2DArray, ObjectHypothesisWithPose

from ultralytics import YOLO
from ultralytics_ros.msg import YoloResult

# 외부 OC-SORT (레포 포크별 경로 호환)
try:
    from trackers.ocsort_tracker.ocsort import OCSort
except Exception:
    try:
        from trackers.ocsort.ocsort import OCSort
    except Exception:
        OCSort = None


class TrackerNode(Node):
    def _ocsort_update_safe(self, dets, H, W):
        """
        dets: (N,5) or (N,6) ndarray [x1,y1,x2,y2,score,(cls)]
        H,W: image size
        서로 다른 포크의 시그니처를 순차 시도.
        """
        try:
            return self.ocsort.update(dets, [H, W], [H, W])  # (dets, img_size, img_info)
        except TypeError:
            try:
                return self.ocsort.update(dets, [H, W])      # (dets, img_size)
            except TypeError:
                return self.ocsort.update(dets)              # (dets)

    def __init__(self):
        super().__init__("tracker_node")

        # ----------------------
        # 파라미터 선언(기본값)
        # ----------------------
        self.declare_parameter("yolo_model", "yolov8n.pt")
        self.declare_parameter("input_topic", "image_raw")
        self.declare_parameter("result_topic", "yolo_result")
        self.declare_parameter("result_image_topic", "yolo_image")
        self.declare_parameter("conf_thres", 0.25)
        self.declare_parameter("iou_thres", 0.45)
        self.declare_parameter("max_det", 300)
        self.declare_parameter("classes", "0") # 문자열로 받고 내부 파싱
        cls_param = self.get_parameter("classes").value
        if isinstance(cls_param, str):
            classes = [int(x) for x in cls_param.strip().split(",") if x != ""]
        elif isinstance(cls_param, (list, tuple)):
           classes = [int(x) for x in cls_param]
        else:
            classes = None
        self.declare_parameter("tracker", "bytetrack.yaml")
        self.declare_parameter("device", "cpu")
        self.declare_parameter("result_conf", True)
        self.declare_parameter("result_line_width", 1)
        self.declare_parameter("result_font_size", 1)
        self.declare_parameter("result_font", "Arial.ttf")
        self.declare_parameter("result_labels", True)
        self.declare_parameter("result_boxes", True)

        # ----------------------
        # YOLO 모델 로드
        # ----------------------
        share_path = get_package_share_directory("ultralytics_ros")
        yolo_model = self.get_parameter("yolo_model").get_parameter_value().string_value
        model_path = (
            yolo_model
            if os.path.isabs(yolo_model) or os.path.exists(yolo_model)
            else f"{share_path}/models/{yolo_model}"
        )
        self.get_logger().info(f"Loading YOLO model: {model_path}")
        self.model = YOLO(model_path)
        self.model.fuse()

        self.bridge = cv_bridge.CvBridge()
        self.use_segmentation = str(yolo_model).endswith("-seg.pt")

        # ----------------------
        # 외부 트래커(OC-SORT) 활성화 여부
        # ----------------------
        self.external_tracker = None
        tracker_param = self.get_parameter("tracker").get_parameter_value().string_value

        if "ocsort" in tracker_param.lower():
            if OCSort is None:
                raise ImportError(
                    "OC-SORT가 설치되지 않았습니다. "
                    "PYTHONPATH에 OC_SORT 루트 추가 후 "
                    "pip 대신 깃 클론 사용하세요: git clone https://github.com/noahcao/OC_SORT.git"
                )
            cfg = {
                "det_thresh": 0.30,
                "iou_threshold": 0.30,
                "max_age": 30,
                "min_hits": 3,
                "delta_t": 3,
                "verbose": False,
            }
            if os.path.exists(tracker_param):
                try:
                    with open(tracker_param, "r") as f:
                        yaml_cfg = yaml.safe_load(f) or {}
                    for k in cfg.keys():
                        if k in yaml_cfg:
                            cfg[k] = yaml_cfg[k]
                except Exception as e:
                    self.get_logger().warn(f"OC-SORT yaml 로드 실패, 기본값 사용: {e}")

            self.ocsort = OCSort(
                det_thresh=cfg["det_thresh"],
                iou_threshold=cfg["iou_threshold"],
                max_age=cfg["max_age"],
                min_hits=cfg["min_hits"],
                delta_t=cfg["delta_t"],
            )
            self.external_tracker = "ocsort"
            self.get_logger().info("External tracker: OC-SORT enabled")

        # ----------------------
        # I/O
        # ----------------------
        input_topic = self.get_parameter("input_topic").get_parameter_value().string_value
        result_topic = self.get_parameter("result_topic").get_parameter_value().string_value
        result_image_topic = self.get_parameter("result_image_topic").get_parameter_value().string_value

        self.create_subscription(Image, input_topic, self.image_callback, 1)
        self.results_pub = self.create_publisher(YoloResult, result_topic, 1)
        self.result_image_pub = self.create_publisher(Image, result_image_topic, 1)

    # =========================
    # 콜백
    # =========================
    def image_callback(self, msg):
        cv_image = self.bridge.imgmsg_to_cv2(msg, desired_encoding="bgr8")

        conf_thres = self.get_parameter("conf_thres").get_parameter_value().double_value
        iou_thres = self.get_parameter("iou_thres").get_parameter_value().double_value
        max_det = self.get_parameter("max_det").get_parameter_value().integer_value
        classes_raw = self.get_parameter("classes").get_parameter_value().string_value.strip()
        classes = [int(x) for x in classes_raw.split(",") if x != ""] if classes_raw else None
        tracker = self.get_parameter("tracker").get_parameter_value().string_value
        device = self.get_parameter("device").get_parameter_value().string_value or None

        if self.external_tracker == "ocsort":
            # 1) 검출만 수행
            results = self.model.predict(
                source=cv_image,
                conf=conf_thres,
                iou=iou_thres,
                max_det=max_det,
                classes=classes,
                device=device,
                verbose=False,
            )
            r0 = results[0]
            boxes = getattr(r0, "boxes", None)

            # 검출 없으면 빈 메시지 + 원본 이미지 퍼블리시
            if boxes is None or boxes.xyxy is None or len(boxes.xyxy) == 0:
                empty = YoloResult()
                empty.header = msg.header
                empty.detections = Detection2DArray()
                self.results_pub.publish(empty)
                self.result_image_pub.publish(self.bridge.cv2_to_imgmsg(cv_image, encoding="bgr8"))
                return

            # dets: [x1,y1,x2,y2,score,cls]
            xyxy = boxes.xyxy.detach().cpu().numpy()
            scores = boxes.conf.detach().cpu().numpy()
            if boxes.cls is None:
                clses = np.zeros(len(xyxy), dtype=int)
            else:
                clses = boxes.cls.detach().cpu().numpy().astype(int)

            dets = np.concatenate([xyxy, scores[:, None], clses[:, None]], axis=1)

            # 2) OC-SORT update (포크별 시그니처 호환)
            H, W = cv_image.shape[:2]
            dets5 = dets[:, :5] if dets.shape[1] >= 5 else dets  # [x1,y1,x2,y2,score]
            tracks = self._ocsort_update_safe(dets5, H, W)

            # 3) 메시지 구성
            yolo_result_msg = YoloResult()
            yolo_result_msg.header = msg.header
            yolo_result_msg.detections = self.create_detections_array_from_tracks(tracks, r0.names)

            # 4) 시각화 (트랙ID 오버레이 필요하면 별도 그리기 추가 가능)
            plotted = r0.plot(conf=True)
            yolo_result_image_msg = self.bridge.cv2_to_imgmsg(plotted, encoding="bgr8")

            if self.use_segmentation and hasattr(r0, "masks") and r0.masks is not None:
                yolo_result_msg.masks = self.create_segmentation_masks(results)

            # publish
            self.results_pub.publish(yolo_result_msg)
            self.result_image_pub.publish(yolo_result_image_msg)
            return

        # ---------------------------
        # 내장 트래커 경로(ByteTrack/BOT-SORT)
        # ---------------------------
        results = self.model.track(
            source=cv_image,
            conf=conf_thres,
            iou=iou_thres,
            max_det=max_det,
            classes=classes,
            tracker=tracker,
            device=device,
            verbose=False,
            retina_masks=True,
        )

        if results is not None:
            yolo_result_msg = YoloResult()
            yolo_result_image_msg = Image()
            yolo_result_msg.header = msg.header
            yolo_result_image_msg.header = msg.header
            yolo_result_msg.detections = self.create_detections_array(results)
            yolo_result_image_msg = self.create_result_image(results)
            if self.use_segmentation:
                yolo_result_msg.masks = self.create_segmentation_masks(results)
            self.results_pub.publish(yolo_result_msg)
            self.result_image_pub.publish(yolo_result_image_msg)

    # =========================
    # 메시지 생성 유틸
    # =========================
    def create_detections_array(self, results):
        detections_msg = Detection2DArray()
        bounding_box = results[0].boxes.xywh
        classes = results[0].boxes.cls
        confidence_score = results[0].boxes.conf
        for bbox, cls, conf in zip(bounding_box, classes, confidence_score):
            det = Detection2D()
            det.bbox.center.position.x = float(bbox[0])
            det.bbox.center.position.y = float(bbox[1])
            det.bbox.size_x = float(bbox[2])
            det.bbox.size_y = float(bbox[3])

            hyp = ObjectHypothesisWithPose()
            hyp.hypothesis.class_id = results[0].names.get(int(cls))
            hyp.hypothesis.score = float(conf)
            det.results.append(hyp)

            detections_msg.detections.append(det)
        return detections_msg

    def create_detections_array_from_tracks(self, tracks, names):
        """
        OC-SORT update() 반환을 Detection2DArray로 변환.
        일반적으로 [x1,y1,x2,y2,track_id,(cls?),(score?)] 형태(포크별 상이) 처리.
        """
        detections_msg = Detection2DArray()
        if tracks is None:
            return detections_msg

        t = np.asarray(tracks)
        if t.size == 0:
            return detections_msg

        for row in t:
            x1, y1, x2, y2 = [float(v) for v in row[:4]]
            cx = (x1 + x2) / 2.0
            cy = (y1 + y2) / 2.0
            w = max(0.0, x2 - x1)
            h = max(0.0, y2 - y1)

            track_id = int(row[4]) if len(row) >= 5 else -1
            cls = int(row[5]) if len(row) >= 6 else 0
            score = float(row[6]) if len(row) >= 7 else 1.0

            det = Detection2D()
            det.bbox.center.position.x = cx
            det.bbox.center.position.y = cy
            det.bbox.size_x = w
            det.bbox.size_y = h

            hyp = ObjectHypothesisWithPose()
            hyp.hypothesis.class_id = names.get(cls, str(cls)) if isinstance(names, dict) else str(cls)
            hyp.hypothesis.score = score
            det.results.append(hyp)

            detections_msg.detections.append(det)
        return detections_msg

    def create_result_image(self, results):
        result_conf = self.get_parameter("result_conf").get_parameter_value().bool_value
        result_line_width = self.get_parameter("result_line_width").get_parameter_value().integer_value
        result_font_size = self.get_parameter("result_font_size").get_parameter_value().integer_value
        result_font = self.get_parameter("result_font").get_parameter_value().string_value
        result_labels = self.get_parameter("result_labels").get_parameter_value().bool_value
        result_boxes = self.get_parameter("result_boxes").get_parameter_value().bool_value

        plotted_image = results[0].plot(
            conf=result_conf,
            line_width=result_line_width,
            font_size=result_font_size,
            font=result_font,
            labels=result_labels,
            boxes=result_boxes,
        )
        return self.bridge.cv2_to_imgmsg(plotted_image, encoding="bgr8")

    def create_segmentation_masks(self, results):
        masks_msg = []
        for result in results:
            if hasattr(result, "masks") and result.masks is not None:
                for mask_tensor in result.masks:
                    mask_numpy = (
                        np.squeeze(mask_tensor.data.to("cpu").detach().numpy()).astype(np.uint8) * 255
                    )
                    mask_image_msg = self.bridge.cv2_to_imgmsg(mask_numpy, encoding="mono8")
                    masks_msg.append(mask_image_msg)
        return masks_msg


def main(args=None):
    rclpy.init(args=args)
    node = TrackerNode()
    rclpy.spin(node)


if __name__ == "__main__":
    main()

