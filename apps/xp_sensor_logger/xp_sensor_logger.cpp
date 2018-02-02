/******************************************************************************
 * Copyright 2017-2018 Baidu Robotic Vision Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <glog/logging.h>
#include <XP/helper/shared_queue.h>
#include <XP/helper/timer.h>
#include <XP/helper/param.h>
#include <XP/helper/tag_detector.h>
#include <driver/xp_aec_table.h>
#include <driver/XP_sensor_driver.h>
#include <XP/util/calibration_utils.h>
#include <XP/util/depth_utils.h>
#include <XP/util/feature_utils.h>
#include <XP/util/image_utils.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#ifdef HAS_OPENCV_VIZ  // defined in CMakeLists
#include <opencv2/viz.hpp>
#endif
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/circular_buffer.hpp>
#include <Eigen/Dense>
#include <algorithm>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
using std::cout;
using std::endl;
using std::vector;
using XPDRIVER::XpSensorMultithread;
using std::chrono::steady_clock;
DEFINE_bool(auto_gain, false, "turn on auto gain");
DEFINE_bool(auto_wb, true, "Whether or not enable auto white balance");
DEFINE_bool(calib_mode, false, "whether or not to show point distribution");
DEFINE_bool(calib_verify, false, "Use april tag board to verify calib result");
DEFINE_string(calib_yaml, "", "load calib file");
DEFINE_bool(depth, false, "whether or not show depth image");
DEFINE_string(dev_id, "", "which dev to open. Empty enables auto mode");
DEFINE_bool(headless, false, "Do not show windows");
DEFINE_bool(horizontal_line, false, "show green horizontal lines for disparity check");
DEFINE_bool(imu_from_image, false, "Load imu from image. Helpful for USB2.0");
DEFINE_bool(orb_verify, false, "Use ORB feature matching to verify calib result");
DEFINE_bool(save_image_bin, false, "Do not save image bin file");
DEFINE_string(sensor_type, "XP", "LI or XP or XP2 or XP3 or FACE or XPIRL or XPIRL2");
DEFINE_bool(show_hist, false, "Show image histogram (left and right)");
DEFINE_bool(spacebar_mode, false, "only save img when press space bar");
DEFINE_string(record_path, "", "path to save images. Set empty to disable saving");
DEFINE_int32(valid_radius, 360,
             "The radius in pixel to check the point coverage from the pinhole center. "
             "Suggested value: 360 for 120 deg FOV and 220 for 170 deg FOV.");
DEFINE_bool(verbose, false, "whether or not log more info");
DEFINE_bool(viz3d, false, "whether or not to turn on point cloud");

#ifdef __ARM_NEON__
DEFINE_int32(cpu_core, 4, "bind program to run on specific core[0 ~ 7],"
             "being out-of-range indicates no binding, only valid on ARM platform");
#endif
struct V4l2BufferData {
  int counter;
  std::shared_ptr<vector<uint8_t>> img_data_ptr;
  V4l2BufferData() {
    img_data_ptr.reset(new vector<uint8_t>);
  }
};
struct ImgForSave {
  cv::Mat l;
  cv::Mat r;
  cv::Mat xyz;
  std::string name;
};
struct StereoImage {
  cv::Mat l;
  cv::Mat r;
  float ts_100us;
};

XP::shared_queue<XPDRIVER::ImuData> imu_data_queue;
XP::shared_queue<ImgForSave> imgs_for_saving_queue;
XP::shared_queue<StereoImage> stereo_image_queue;
std::atomic<bool> run_flag;

// we use the first imu to approx img time based on img counter
std::atomic<bool> g_auto_gain;
std::atomic<bool> g_auto_infrared;
int g_aec_index;  // signed int as the index may go to negative while calculation
int g_infrared_index;
cv::Size g_img_size;
// The unique instance of XpSensorMultithread
std::unique_ptr<XPDRIVER::XpSensorMultithread> g_xp_sensor_ptr;
XP::AprilTagDetector g_ap_detector;
cv::BFMatcher g_orb_matcher(cv::NORM_HAMMING);

// Callback functions for XpSensorMultithread
// [NOTE] These callback functions have to be light-weight as it *WILL* block XpSensorMultithread
void image_data_callback(const cv::Mat& img_l, const cv::Mat& img_r, const float ts_100us) {
  if (run_flag) {
    StereoImage stereo_img;
    stereo_img.l = img_l;
    stereo_img.r = img_r;
    stereo_img.ts_100us = ts_100us;
    stereo_image_queue.push_back(stereo_img);
  }
}

void imu_data_callback(const XPDRIVER::ImuData& imu_data) {
  if (run_flag) {
    imu_data_queue.push_back(imu_data);
  }
}

// Utility functions
void verify_calibration(const vector<cv::Mat_<uchar>>& cam_mask_lr,
                        const vector<cv::Matx34f>& proj_mat_lr,
                        const XP::DuoCalibParam& calib_param,
                        const cv::Mat& img_l_mono,
                        const cv::Mat& img_r_mono,
                        cv::Mat* img_l_display_ptr,
                        cv::Mat* img_r_display_ptr) {
  CHECK_NOTNULL(img_l_display_ptr);
  CHECK_NOTNULL(img_r_display_ptr);
  cv::Mat& img_l_display = *img_l_display_ptr;
  cv::Mat& img_r_display = *img_r_display_ptr;

  int det_count_l = 0;
  int det_count_r = 0;
  int match_count = 0;
  int less_than_1_count = 0;
  int less_than_2_count = 0;

  vector<vector<cv::Point2f>> matched_raw_pnts(2);
  if (FLAGS_calib_verify) {
    vector<vector<cv::KeyPoint>> apt_lr(2);
    g_ap_detector.detect(img_l_mono, &(apt_lr[0]));
    g_ap_detector.detect(img_r_mono, &(apt_lr[1]));
    // Plot marker to img
    for (const auto& kp : apt_lr[0]) {
      cv::circle(img_l_display, kp.pt, 2, cv::Scalar(255, 255, 255));
    }
    for (const auto& kp : apt_lr[1]) {
      cv::circle(img_r_display, kp.pt, 2, cv::Scalar(255, 255, 255));
    }
    det_count_l = apt_lr[0].size();
    det_count_r = apt_lr[1].size();
    if (!apt_lr[0].empty() && !apt_lr[1].empty()) {
      matched_raw_pnts[0].reserve(apt_lr[0].size());
      matched_raw_pnts[1].reserve(apt_lr[1].size());
      for (size_t it_l = 0; it_l < apt_lr[0].size(); ++it_l) {
        CHECK_GE(apt_lr[0][it_l].class_id, 0);
        for (size_t it_r = 0; it_r < apt_lr[1].size(); ++it_r) {
          if (apt_lr[1][it_r].class_id == apt_lr[0][it_l].class_id) {
            // we got a matched april tag
            ++match_count;
            matched_raw_pnts[0].push_back(apt_lr[0][it_l].pt);
            matched_raw_pnts[1].push_back(apt_lr[1][it_r].pt);
            break;
          }
        }
      }
    }
  } else if (FLAGS_orb_verify) {
    // same as VioMapperBase::count_small_error_stereo_match
    vector<vector<cv::KeyPoint>> kp_lr(2);
    vector<cv::Mat> orb_lr(2);
    const int request_feat_num = 200;
    const int fast_thresh = 15;
    XP::detect_orb_features(img_l_mono,
                            cam_mask_lr[0],
                            request_feat_num,
                            2,  // pyra_level,
                            fast_thresh,
                            true,  // use_fast (or TomasShi)
                            5,  // enforce_uniformatiy_radius (less than 5 means no enforcement)
                            &kp_lr[0],
                            &orb_lr[0]);
    det_count_l = kp_lr[0].size();
    XP::detect_orb_features(img_r_mono,
                            cam_mask_lr[1],
                            request_feat_num,
                            2,  // pyra_level,
                            fast_thresh,
                            true,  // use_fast ( or TomasShi)
                            5,  // enforce_uniformatiy_radius (less than 5 means no enforcement)
                            &kp_lr[1],
                            &orb_lr[1]);
    det_count_r = kp_lr[1].size();
    if (!kp_lr[0].empty() && !kp_lr[1].empty()) {
      // matching
      vector<vector<vector<cv::DMatch>>> matches_lr(2);
      vector<cv::Mat> matching_mask_lr(2);
      matching_mask_lr[0].create(orb_lr[0].rows, orb_lr[1].rows, CV_8U);
      matching_mask_lr[0].setTo(0x01);
      matching_mask_lr[1].create(orb_lr[1].rows, orb_lr[0].rows, CV_8U);
      matching_mask_lr[1].setTo(0x01);
      g_orb_matcher.knnMatch(orb_lr[0], orb_lr[1], matches_lr[0], 1, matching_mask_lr[0]);
      g_orb_matcher.knnMatch(orb_lr[1], orb_lr[0], matches_lr[1], 1, matching_mask_lr[1]);
      // cross validation
      vector<vector<bool>> is_matched_lr(2);
      for (int  lr = 0; lr < 2; ++lr) {
        is_matched_lr[lr].resize(orb_lr[lr].rows, false);
        matched_raw_pnts[lr].reserve(orb_lr[lr].rows);
        for (auto& kp : kp_lr[lr]) {
          kp.class_id = -1;
        }
      }
      for (int it_pnt_0 = 0; it_pnt_0 < orb_lr[0].rows; ++it_pnt_0) {
        const int match_id_in_1 = matches_lr[0][it_pnt_0][0].trainIdx;
        const int match_id_in_0 = matches_lr[1][match_id_in_1][0].trainIdx;
        if (match_id_in_0 == it_pnt_0) {
          kp_lr[0][it_pnt_0].class_id = match_id_in_1;
          kp_lr[1][match_id_in_1].class_id = it_pnt_0;
          ++match_count;
          matched_raw_pnts[0].push_back(kp_lr[0][it_pnt_0].pt);
          matched_raw_pnts[1].push_back(kp_lr[1][match_id_in_1].pt);
        }
      }
      // Plot marker to img
      for (const auto& kp : kp_lr[0]) {
        if (kp.class_id < 0) {  // No match
          cv::circle(img_l_display, kp.pt, 2, cv::Scalar(255, 255, 255));
        } else {
          cv::circle(img_l_display, kp.pt, 2, cv::Scalar(0, 255, 255));
        }
      }
      for (const auto& kp : kp_lr[1]) {
        if (kp.class_id < 0) {  // No match
          cv::circle(img_r_display, kp.pt, 2, cv::Scalar(255, 255, 255));
        } else {
          cv::circle(img_r_display, kp.pt, 2, cv::Scalar(0, 255, 255));
        }
      }
    }
  }
  std::stringstream ss;
  if (match_count == 0) {
    ss << "score N/A";
  } else {
    CHECK_EQ(matched_raw_pnts[0].size(), match_count);
    CHECK_EQ(matched_raw_pnts[1].size(), match_count);
    vector<vector<cv::Point2f>> matched_pnts(2);
    for (int lr = 0; lr < 2; ++lr) {
      cv::undistortPoints(matched_raw_pnts[lr],
                          matched_pnts[lr],
                          calib_param.Camera.cv_camK_lr[lr],
                          calib_param.Camera.cv_dist_coeff_lr[lr]);
    }
    cv::Mat homo_pnts_3d;
    cv::triangulatePoints(proj_mat_lr[0], proj_mat_lr[1],
                          matched_pnts[0], matched_pnts[1], homo_pnts_3d);
    CHECK_EQ(homo_pnts_3d.type(), CV_32F);
    CHECK_EQ(homo_pnts_3d.rows, 4);
    CHECK_EQ(homo_pnts_3d.cols, match_count);
    vector<cv::Point3f> pnts3d(match_count);
    for (int i = 0; i < match_count; ++i) {
      pnts3d[i].x = homo_pnts_3d.at<float>(0, i) / homo_pnts_3d.at<float>(3, i);
      pnts3d[i].y = homo_pnts_3d.at<float>(1, i) / homo_pnts_3d.at<float>(3, i);
      pnts3d[i].z = homo_pnts_3d.at<float>(2, i) / homo_pnts_3d.at<float>(3, i);
    }
    vector<cv::Point2f> projs_l;
    cv::projectPoints(pnts3d,
                      cv::Matx31d::zeros(),
                      cv::Matx31d::zeros(),
                      calib_param.Camera.cv_camK_lr[0],
                      calib_param.Camera.cv_dist_coeff_lr[0],
                      projs_l);
    CHECK_EQ(projs_l.size(), match_count);
    CHECK_EQ(matched_pnts[0].size(), match_count);
    for (int i = 0; i < match_count; ++i) {
      if (pnts3d[i].z > 0) {
        const float diff_x = projs_l[i].x - matched_raw_pnts[0][i].x;
        const float diff_y = projs_l[i].y - matched_raw_pnts[0][i].y;
        if (diff_x * diff_x + diff_y * diff_y < 1) {
          ++less_than_1_count;
          ++less_than_2_count;
          cv::circle(img_l_display, matched_raw_pnts[0][i], 5, cv::Scalar(0, 255, 0));
        } else if (diff_x * diff_x + diff_y * diff_y < 4) {
          ++less_than_2_count;
        } else {
          if (FLAGS_calib_verify) {
            cv::line(img_l_display, projs_l[i], matched_raw_pnts[0][i], cv::Scalar(0, 0, 255));
          } else {
            // There must be some matching false alarms.
            // Don't show obviously wrong matches
            if (diff_x * diff_x + diff_y * diff_y < 900) {
              cv::line(img_l_display, projs_l[i], matched_raw_pnts[0][i], cv::Scalar(0, 0, 255));
            }
          }
        }
      }
    }
    // Compute the heuristic calibration score
    int score;
    if (FLAGS_calib_verify) {
      score = 100 * less_than_1_count / match_count;
    } else {
      // the guess is that there should at least 20 features that are good match
      if (less_than_1_count > 20) {
        score = 100;
      } else {
        score = 100 * less_than_1_count / std::max(30, match_count);
      }
    }
    ss << "score " << score << " reproj < 1 (2) " << less_than_1_count << "(" << less_than_2_count
       << ") match # " << match_count << " det " << det_count_l << " / " << det_count_r;
  }
  cv::putText(img_l_display, ss.str(),
              cv::Point(15, 35), cv::FONT_HERSHEY_COMPLEX, 0.5,
              cv::Scalar(255, 0, 255), 1);
}

cv::Mat_<cv::Vec3f> g_depth_xyz_img;
cv::Mat g_disparity_img;
cv::Mat g_disparity_buf;  // for filterSpeckles
vector<cv::Mat> g_disparity_ml;
void process_stereo_depth(const XP::DuoCalibParam& calib_param,
                          const cv::Mat& img_l_mono,
                          const cv::Mat& img_r_mono,
                          const bool save_img,
#ifdef HAS_OPENCV_VIZ
                          cv::Mat* depth_canvas,
                          cv::viz::Viz3d* viz_window) {
#else
                          cv::Mat* depth_canvas) {
#endif  // HAS_OPENCV_VIZ
  // Sanity check
  CHECK_EQ(img_l_mono.channels(), 1);
  CHECK_EQ(img_r_mono.channels(), 1);
  CHECK_EQ(img_l_mono.type(), CV_8U);
  CHECK_EQ(img_r_mono.type(), CV_8U);
  XP::multilevel_stereoBM(calib_param,
                          img_l_mono,
                          img_r_mono,
                          &g_disparity_img,
                          &g_disparity_ml,
                          0,
                          2,
                          &g_disparity_buf);
  CHECK_EQ(g_disparity_img.type(), CV_16SC1);
  CHECK_GT(g_disparity_img.rows, 0);
  CHECK_GT(g_disparity_img.cols, 0);
  // compute xyz img for display or saving
  if (save_img
#ifdef HAS_OPENCV_VIZ
      || (FLAGS_viz3d && !FLAGS_headless)
#endif
      ) {
    if (g_depth_xyz_img.rows == 0) {
      g_depth_xyz_img.create(g_disparity_img.rows, g_disparity_img.cols);
    }
    g_depth_xyz_img.setTo(cv::Vec3f(0, 0, 0));
    for (int y = 0; y < g_disparity_img.rows; ++y) {
      for (int x = 0; x < g_disparity_img.cols; ++x) {
        int16_t disp = g_disparity_img.at<int16_t>(y, x);
        if (disp <= 0) continue;
        // http://docs.opencv.org/3.0.0/d9/d0c/group__calib3d.html#ga1bc1152bd57d63bc524204f21fde6e02
        // [XYZW]T=𝚀∗[x y 𝚍𝚒𝚜𝚙𝚊𝚛𝚒𝚝𝚢(x,y) 1]T
        cv::Vec4f xyz_homo = calib_param.Camera.Q *
            cv::Vec4f(x, y, static_cast<float>(disp) / 16.f, 1);
        cv::Vec3f xyz_C(xyz_homo[0] / xyz_homo[3],
                        xyz_homo[1] / xyz_homo[3],
                        xyz_homo[2] / xyz_homo[3]);
        g_depth_xyz_img.at<cv::Vec3f>(y, x) = xyz_C;
      }
    }
  }
  if (!FLAGS_headless) {
    if (depth_canvas->rows == 0) {
      depth_canvas->create(g_disparity_img.size(), CV_8UC3);
    }
    for (int i = 0; i < g_disparity_img.rows; ++i) {
      for (int j = 0; j < g_disparity_img.cols; ++j) {
        depth_canvas->at<cv::Vec3b>(i, j) = XP::depth16S2color(g_disparity_img.at<int16_t>(i, j));
      }
    }
    imshow("depth_canvas", *depth_canvas);

#ifdef HAS_OPENCV_VIZ
    // compute xyz
    if (FLAGS_viz3d) {
      // compute color based on Z val
      cv::Mat_<cv::Vec3b> color(g_depth_xyz_img.size());
      constexpr float far_cut = 1.f;
      constexpr float near_cut = 0.2f;
      for (int x = 0; x < g_depth_xyz_img.cols; ++x) {
        for (int y = 0; y < g_depth_xyz_img.rows; ++y) {
          const float z_val = g_depth_xyz_img(y, x)[2];
          if (z_val > far_cut) {
            g_depth_xyz_img.at<cv::Vec3f>(y, x) = cv::Vec3f(0, 0, 0);
            color(y, x) = cv::Vec3b(0, 0xff, 0);
          } else if (z_val < near_cut) {
            g_depth_xyz_img.at<cv::Vec3f>(y, x) = cv::Vec3f(0, 0, 0);
            color(y, x) = cv::Vec3b(0, 0, 0);
          } else {
            color(y, x)[2] =
                static_cast<uint8_t>(255 * (far_cut - z_val) / (far_cut - near_cut));
            color(y, x)[1] =
                static_cast<uint8_t>(255 * (z_val - near_cut) / (far_cut - near_cut));
            color(y, x)[0] = 0;
          }
        }
      }
      cv::viz::WCloud pnt_cloud(g_depth_xyz_img, color);
      viz_window->showWidget("depth img", pnt_cloud);
      // A long spin time to make sure the pnt clound is displayed
      viz_window->spinOnce(5, false);
    }
#endif
  }
}

bool kill_all_shared_queues() {
  imgs_for_saving_queue.kill();
  stereo_image_queue.kill();
  imu_data_queue.kill();
  return true;
}

bool process_infrared_pwm(char keypressed) {
  if (g_xp_sensor_ptr == nullptr) {
    return false;
  }
  using XPDRIVER::XP_SENSOR::infrared_pwm_max;
  if (g_auto_infrared && keypressed != -1) {  // -1 means no key is pressed
    switch (keypressed) {
      case '6':
        g_infrared_index = 1;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '7':
        g_infrared_index = infrared_pwm_max * 0.2;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '8':
        g_infrared_index = infrared_pwm_max * 0.6;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '9':
        g_infrared_index = infrared_pwm_max - 10;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '.':
        ++g_infrared_index;
        if (g_infrared_index >= infrared_pwm_max) g_infrared_index = infrared_pwm_max -1;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '>':
        g_infrared_index += 5;
        if (g_infrared_index >= infrared_pwm_max) g_infrared_index = infrared_pwm_max -1;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case ',':
        --g_infrared_index;
        if (g_infrared_index < 0) g_infrared_index = 0;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      case '<':
        g_infrared_index -= 5;
        if (g_infrared_index < 0) g_infrared_index = 0;
        g_xp_sensor_ptr->set_infrared_index(g_infrared_index);
        break;
      default:
        break;
    }
  }
  // only XPIRL support infrared light
  // TODO(zhoury) XPIRL2 has not support infrared light
  if (FLAGS_sensor_type == "XPIRL" && (keypressed == 'i' || keypressed == 'I')) {
    g_auto_infrared = !g_auto_infrared;
    std::cout << "set infrared mode:" << ((g_auto_infrared == true) ? "on" : "off") << std::endl;
    // TODO(zhourenyi): add auto pwm function
    if (g_auto_infrared == false)
      g_xp_sensor_ptr->set_infrared_index(0);
    g_xp_sensor_ptr->set_auto_infrared(g_auto_infrared);
  }
  return true;
}
bool process_gain_control(char keypressed) {
  if (FLAGS_sensor_type == "LI") {
    return false;
  }
  if (g_xp_sensor_ptr == nullptr) {
    return false;
  }
  using XPDRIVER::XP_SENSOR::kAEC_steps;
  if (!g_auto_gain && keypressed != -1) {  // -1 means no key is pressed
    switch (keypressed) {
      case '1':
        // The lowest brightness possible
        g_aec_index = 0;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '2':
        // 20% of max brightness
        g_aec_index = kAEC_steps * 0.2;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '3':
        // 60% of max brightness
        g_aec_index = kAEC_steps * 0.6;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '4':
        // max brightness
        g_aec_index = kAEC_steps - 1;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '+':
      case '=':
        ++g_aec_index;
        if (g_aec_index >= kAEC_steps) g_aec_index = kAEC_steps - 1;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case ']':
        g_aec_index += 5;
        if (g_aec_index >= kAEC_steps) g_aec_index = kAEC_steps - 1;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '-':
        --g_aec_index;
        if (g_aec_index < 0) g_aec_index = 0;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      case '[':
        g_aec_index -= 5;
        if (g_aec_index < 0) g_aec_index = 0;
        g_xp_sensor_ptr->set_aec_index(g_aec_index);
        break;
      default:
        break;
    }
  }
  if (keypressed == 'a' || keypressed == 'A') {
    g_auto_gain = !g_auto_gain;
    g_xp_sensor_ptr->set_auto_gain(g_auto_gain);
  }
  return true;
}

// Threads
void thread_proc_img() {
  VLOG(1) << "========= thread_proc_img thread starts";
  // mode compatibility is done in main
  cv::Mat img_lr_display, img_l_display, img_r_display;
  // cv::namedWindow has to be used in a single place
  if (!FLAGS_headless) {
    cv::namedWindow("img_lr");
    cv::moveWindow("img_lr", 1, 1);
    img_lr_display.create(g_img_size.height, g_img_size.width * 2, CV_8UC3);
    img_l_display = img_lr_display(cv::Rect(0, 0, g_img_size.width, g_img_size.height));
    img_r_display = img_lr_display(cv::Rect(g_img_size.width, 0,
                                            g_img_size.width, g_img_size.height));
    if (FLAGS_depth) {
      cv::namedWindow("depth_canvas");
      cv::moveWindow("depth_canvas", g_img_size.width * 2, 1);
    }
    if (FLAGS_calib_mode) {
      cv::namedWindow("coverage l");
      cv::moveWindow("coverage l", 1, g_img_size.height);
      cv::namedWindow("coverage r");
      cv::moveWindow("coverage r", g_img_size.width, g_img_size.height);
    }
  }
  const bool is_color = g_xp_sensor_ptr->is_color();

#ifdef HAS_OPENCV_VIZ
  cv::viz::Viz3d viz_window;
  if (FLAGS_depth && !FLAGS_headless && FLAGS_viz3d) {
    viz_window = cv::viz::Viz3d("depth");
    viz_window.setBackgroundColor(cv::viz::Color::bluberry());
  }
#endif
  cv::Mat depth_canvas;
  cv::Mat hist_canvas(256, 256, CV_8UC3);
  cv::Mat hist_canvas_l = hist_canvas(cv::Rect(0, 0,
                                               hist_canvas.cols,
                                               hist_canvas.rows / 2));
  cv::Mat hist_canvas_r = hist_canvas(cv::Rect(0, hist_canvas.rows / 2,
                                               hist_canvas.cols,
                                               hist_canvas.rows / 2));
  cv::Ptr<cv::StereoBM> bm_matcher = cv::StereoBM::create(96, /* numDisparities divisble by 16*/
                                                          21  /* blockSize */);
  XP::DuoCalibParam calib_param;
  if (!FLAGS_calib_yaml.empty()) {
    if (!calib_param.LoadCamCalibFromYaml(FLAGS_calib_yaml)) {
      LOG(ERROR) << FLAGS_calib_yaml << " cannot be loaded";
      return;
    }
    if (g_img_size != calib_param.Camera.img_size) {
      LOG(ERROR) << "g_img_size = " << g_img_size
      << " != calib info" << calib_param.Camera.img_size;
      return;
    }
  }
  vector<cv::Matx34f> proj_mat_lr(2);
  if (FLAGS_calib_verify || FLAGS_orb_verify) {
    CHECK(!FLAGS_calib_yaml.empty());
    CHECK_EQ(calib_param.Camera.D_T_C_lr.size(), 2);
    for (int lr = 0; lr < 2; ++lr) {
      Eigen::Matrix4f C_T_W = calib_param.Camera.D_T_C_lr[lr].inverse();
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
          proj_mat_lr[lr](i, j) = C_T_W(i, j);
        }
      }
    }
  }

  // These image Mat will be assigned properly according to the sensor type
  cv::Mat img_l_mono, img_r_mono, img_l_color, img_r_color;

  // for ORB detector
  vector<cv::Mat_<uchar>> cam_mask_lr(2);
  if (!FLAGS_calib_yaml.empty()) {
    for (int lr = 0; lr < 2; ++lr) {
      float fov_deg;
      CHECK(XP::generate_cam_mask(calib_param.Camera.cv_camK_lr[lr],
                                  calib_param.Camera.cv_dist_coeff_lr[lr],
                                  calib_param.Camera.img_size,
                                  &cam_mask_lr[lr],
                                  &fov_deg));
      LOG(INFO) << "Generate cam " << lr << " mask (fov: " << fov_deg << " deg)";
    }
  }

  size_t frame_counter = 0;
  std::chrono::time_point<steady_clock> pre_proc_time = steady_clock::now();
  float thread_proc_img_rate = 0.f;
  while (run_flag) {
    VLOG(1) << "========= thread_proc_img loop starts";
    // check if the imgs queue is too long
    bool pop_to_back = FLAGS_calib_verify || FLAGS_orb_verify;
    if (stereo_image_queue.size() > 10) {
      pop_to_back = true;
      if (!FLAGS_depth) {
        // only show error if no additional computation is needed
        LOG(ERROR) << "stereo_image_queue too long (" << stereo_image_queue.size()
                   << "). Pop to back";
      }
    }
    StereoImage stereo_img;
    if (pop_to_back) {
      // record and calib_verify cannot be set at the same time
      if (!stereo_image_queue.wait_and_pop_to_back(&stereo_img)) {
        break;
      }
      VLOG(1) << "stereo_image_queue.wait_and_pop_front done";
    } else {
      if (!stereo_image_queue.wait_and_pop_front(&stereo_img)) {
        break;
      }
      VLOG(1) << "stereo_image_queue.wait_and_pop_front done";
    }

    // Compute the processing rate
    if (frame_counter % 10 == 0) {
      const int ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          steady_clock::now() - pre_proc_time).count();
      pre_proc_time = steady_clock::now();
      if (ms > 0) {
        thread_proc_img_rate = 10 * 1000  / ms;
      }
    }

    // Get the mono/color image Mats properly
    // Sanity check first
    if (is_color) {
      CHECK_EQ(stereo_img.l.type(), CV_8UC3);  // sanity check
      img_l_color = stereo_img.l;
      img_r_color = stereo_img.r;
      cv::cvtColor(img_l_color, img_l_mono, cv::COLOR_BGR2GRAY);
      cv::cvtColor(img_r_color, img_r_mono, cv::COLOR_BGR2GRAY);
    } else {
      CHECK_EQ(stereo_img.l.type(), CV_8UC1);  // sanity check
      img_l_mono = stereo_img.l;
      img_r_mono = stereo_img.r;
      cv::cvtColor(img_l_mono, img_l_color, cv::COLOR_GRAY2BGR);
      cv::cvtColor(img_r_mono, img_r_color, cv::COLOR_GRAY2BGR);
    }

    bool save_img = false;
    if (!FLAGS_spacebar_mode && !FLAGS_record_path.empty()) {
      // always true
      save_img = true;
    }
    char keypressed = cv::waitKey(10);
    if (keypressed == 27) {
      // ESC
      kill_all_shared_queues();
      run_flag = false;
      break;
    } else if (keypressed == 32 && !FLAGS_record_path.empty()) {
      // space
      save_img = true;
    } else if (keypressed != -1) {
      if (!process_gain_control(keypressed)) {
        LOG(ERROR) << "cannot process gain control";
      }
      if (!process_infrared_pwm(keypressed)) {
        LOG(ERROR) << "cannot process infrared control";
      }
    }
    if (!FLAGS_headless) {
      img_l_color.copyTo(img_l_display);
      img_r_color.copyTo(img_r_display);

      if (FLAGS_calib_verify || FLAGS_orb_verify) {
        verify_calibration(cam_mask_lr,
                           proj_mat_lr,
                           calib_param,
                           img_l_mono,
                           img_r_mono,
                           &img_l_display,
                           &img_r_display);
      }
      if (FLAGS_show_hist) {
        // TODO(mingyu): Improve to show color histogram
        vector<int> histogram;
        if (XP::sampleBrightnessHistogram(img_l_mono, &histogram)) {
          XP::drawHistogram(&hist_canvas_l, histogram);
        }
        if (XP::sampleBrightnessHistogram(img_r_mono, &histogram)) {
          XP::drawHistogram(&hist_canvas_r, histogram);
        }
        imshow("histogram", hist_canvas);
      }
    }

    if (FLAGS_depth) {
      process_stereo_depth(calib_param,
                           img_l_mono,
                           img_r_mono,
                           save_img,
#ifdef HAS_OPENCV_VIZ
                           &depth_canvas,
                           &viz_window);
#else
                           &depth_canvas);
#endif  // HAS_OPENCV_VIZ
    }
    // show some debug info
    std::string debug_string;
    float img_rate = g_xp_sensor_ptr->get_image_rate();
    float imu_rate = g_xp_sensor_ptr->get_imu_rate();
    char buf[100];
    snprintf(buf, sizeof(buf),
             "img %4.1f Hz imu %5.1f Hz proc %4.1f Hz time %.2f sec",
             img_rate, imu_rate, thread_proc_img_rate, stereo_img.ts_100us * 1e-4);
    debug_string = std::string(buf);
    if (!FLAGS_headless) {
      if (FLAGS_horizontal_line) {
        // Undistort the image before showing the horizontal line
        cv::Mat img_undistorted(g_img_size.height, g_img_size.width, CV_8UC3);
        cv::remap(img_l_color, img_undistorted,
                  calib_param.Camera.undistort_map_op1_lr[0],
                  calib_param.Camera.undistort_map_op2_lr[0], cv::INTER_LINEAR);
        img_undistorted.copyTo(img_l_display);
        cv::remap(img_r_color, img_undistorted,
                  calib_param.Camera.undistort_map_op1_lr[1],
                  calib_param.Camera.undistort_map_op2_lr[1], cv::INTER_LINEAR);
        img_undistorted.copyTo(img_r_display);
        for (int r = 20; r < img_lr_display.rows; r += 20) {
          cv::line(img_lr_display,
                   cv::Point2i(0, r),
                   cv::Point2i(img_lr_display.cols - 1, r),
                   cv::Scalar(0, 255, 0));
        }
      }
      cv::putText(img_l_display, debug_string,
                  cv::Point(15, 15), cv::FONT_HERSHEY_COMPLEX, 0.5,
                  cv::Scalar(255, 0, 255), 1);

#ifdef __ARM_NEON__
      if ((frame_counter & 1) == 0) {
        imshow("img_lr", img_lr_display);
      }
#else
      // imshow("img_lr", img_lr_display);
      imshow("img_lr", img_lr_display);
#endif
    } else {
      // FLAGS_headless is true
      std::cout << debug_string << std::endl;
    }

    if (save_img) {
      uint64_t img_time_100us = static_cast<uint64_t>(stereo_img.ts_100us);
      std::ostringstream ss;
      ss << std::setfill('0') << std::setw(10) << img_time_100us;
      ImgForSave img_for_save;
      img_for_save.name = ss.str();
      img_for_save.l = stereo_img.l.clone();  // The channels are mono: 1, color: 3
      img_for_save.r = stereo_img.r.clone();  // The channels are mono: 1, color: 3
      if (FLAGS_depth) {
        img_for_save.xyz = g_depth_xyz_img.clone();
      }
      imgs_for_saving_queue.push_back(img_for_save);
    }
    ++frame_counter;
    // waitKey will be used in the next loop
    usleep(1000);  // sleep for 1ms
    VLOG(1) << "========= thread_proc_img loop ends";
  }
  VLOG(1) << "========= thread_proc_img stops";
}

void thread_save_img() {
  if (FLAGS_record_path.empty()) {
    return;
  }
  namespace fs = boost::filesystem;
  fs::create_directory(fs::path(FLAGS_record_path) / "l");
  fs::create_directory(fs::path(FLAGS_record_path) / "r");
  if (FLAGS_depth) {
    fs::create_directory(fs::path(FLAGS_record_path) / "Z");
  }
  cv::Mat cache_img_l, cache_img_r;
  // for verify point coverage
  vector<vector<vector<cv::Point2f>>> detected_corners_lr;
  detected_corners_lr.resize(2);
  detected_corners_lr[0].reserve(10000);
  detected_corners_lr[1].reserve(10000);
  vector<cv::Mat> visualize_img_lr(2);
  while (run_flag) {
    ImgForSave img_for_save;
    if (!imgs_for_saving_queue.wait_and_pop_to_back(&img_for_save)) {
      break;
    }
    cv::imwrite(FLAGS_record_path + "/l/" + img_for_save.name + ".png", img_for_save.l);
    cv::imwrite(FLAGS_record_path + "/r/" + img_for_save.name + ".png", img_for_save.r);
    if (img_for_save.xyz.rows > 0) {
      // save Z val
      cv::Mat_<uint16_t> z_img(img_for_save.xyz.size());
      for (int i = 0; i < img_for_save.xyz.rows; ++i) {
        for (int j = 0; j < img_for_save.xyz.cols; ++j) {
          if (img_for_save.xyz.at<cv::Vec3f>(i, j)[2] < 1e-5) {
            z_img(i, j) = 0;
          } else {
            // convert to mm
            z_img(i, j) = img_for_save.xyz.at<cv::Vec3f>(i, j)[2] * 1000;
          }
        }
      }
      cv::imwrite(FLAGS_record_path + "/Z/" + img_for_save.name + ".png", z_img);
    }
    // verify coverage if calib_mode is on
    // spacebar mode is always on if FLAGS_calib_mode == true
    if (FLAGS_calib_mode) {
      vector<vector<cv::KeyPoint>> apt_lr(2);
      cv::Mat mono_l, mono_r;
      if (img_for_save.l.channels() == 1) {
        mono_l = img_for_save.l;
      } else {
        cv::cvtColor(img_for_save.l, mono_l, cv::COLOR_BGR2GRAY);
      }
      if (img_for_save.r.channels() == 1) {
        mono_r = img_for_save.r;
      } else {
        cv::cvtColor(img_for_save.r, mono_r, cv::COLOR_BGR2GRAY);
      }
      g_ap_detector.detect(mono_l, &(apt_lr[0]));
      g_ap_detector.detect(mono_r, &(apt_lr[1]));
      for (int lr = 0; lr < 2; ++lr) {
        vector<cv::Point2f> pnts_2f(apt_lr[lr].size());
        for (size_t it = 0; it < apt_lr[lr].size(); ++it) {
          pnts_2f[it] = apt_lr[lr][it].pt;
        }
        detected_corners_lr[lr].push_back(pnts_2f);
        if (visualize_img_lr[lr].rows == 0) {
          visualize_img_lr[lr].create(g_img_size, CV_8UC3);
        }
        visualize_img_lr[lr].setTo(cv::Vec3b(0, 0, 0));
        std::ostringstream ss;
        ss << "img # " << detected_corners_lr[lr].size();
        if (detected_corners_lr.size() > 100) {
          ss << "Too many images";
        }
        cv::putText(visualize_img_lr[lr], ss.str(),
                    cv::Point(15, 35), cv::FONT_HERSHEY_COMPLEX, 0.5,
                    cv::Scalar(255, 0, 255), 1);
        for (const vector<cv::Point2f>& detected_corners : detected_corners_lr[lr]) {
          for (const cv::Point2f& pt : detected_corners) {
            // it is possible april tag returns -1 to size + 1 results
            CHECK_GE(pt.x, -1);
            CHECK_LT(pt.x, g_img_size.width + 1);
            CHECK_GE(pt.y, -1);
            CHECK_LT(pt.y, g_img_size.height + 1);
            visualize_img_lr[lr].at<cv::Vec3b>(static_cast<int>(pt.y),
                                               static_cast<int>(pt.x)) = cv::Vec3b(0, 0xff, 0);
          }
        }
        XP::check_grid_point_density(detected_corners_lr[lr],
                                     g_img_size,
                                     0.4,  // const float min_ratio,
                                     FLAGS_valid_radius,  // const int valid_radius,
                                     cv::Point2f(g_img_size.width / 2,
                                                 g_img_size.height / 2),
                                     FLAGS_verbose,  // verbose,
                                     &visualize_img_lr[lr]);
      }
      if (!FLAGS_headless) {
        imshow("coverage l", visualize_img_lr[0]);
        imshow("coverage r", visualize_img_lr[1]);
      }
    }
    VLOG(1) << "========= thread_save_img loop ends";
  }
  VLOG(1) << "========= thread_save_img thread stops";
}
void thread_write_imu_data() {
  VLOG(1) << "========= thread_write_imu_data thread starts";
  // write imu data
  std::ofstream imu_fstream;
  if (!FLAGS_record_path.empty()) {
    imu_fstream.open((FLAGS_record_path + "/imu_data.txt").c_str(), std::iostream::trunc);
    if (imu_fstream.is_open()) {
      cout << "write to " << FLAGS_record_path + "/imu_data.txt " << endl;
    } else {
      cout << "Fail to open " << FLAGS_record_path + "/imu_data.txt " << endl;
    }
  }
  while (run_flag) {
    XPDRIVER::ImuData imu_data;
    if (!imu_data_queue.wait_and_pop_front(&imu_data)) {
      break;
    }
    if (imu_fstream.is_open()) {
      const int temperature = 999;  // a fake value
      // The imu timestamp is in 100us
      // accel is in m/s^2
      // angv is in rad/s
      imu_fstream << imu_data.time_stamp << " "
                  << imu_data.accel[0] << " "
                  << imu_data.accel[1] << " "
                  << imu_data.accel[2] << " "
                  << imu_data.ang_v[0] << " "
                  << imu_data.ang_v[1] << " "
                  << imu_data.ang_v[2] << " "
                  << temperature << endl;
    }
  }
  if (imu_fstream.is_open()) {
    imu_fstream.close();
  }
  VLOG(1) << "========= thread_write_imu_data thread ends";
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

#ifdef __ARM_NEON__
  if (FLAGS_cpu_core >= 0 && FLAGS_cpu_core < 8) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(FLAGS_cpu_core, &set);

    if (0 != sched_setaffinity(getpid(), sizeof(cpu_set_t), &set))
      exit(1);
    std::cout << "RUN ON CORE [" << FLAGS_cpu_core << "]" << std::endl;
  }
#endif  // __ARM_NEON__
#ifdef __linux__  // predefined by gcc
  const char* env_display_p = std::getenv("DISPLAY");
  if (!FLAGS_headless && env_display_p == nullptr) {
    std::cout << "You are running headless OS. No window will be shown" << std::endl;
    FLAGS_headless = true;
  }
#endif
  if (FLAGS_calib_mode) {
    FLAGS_spacebar_mode = true;  // only record images at pressing
    if (FLAGS_record_path.empty()) {
      LOG(ERROR) << "calib_mode is set but record_path is not provided";
      return -1;
    }
  }
  if (FLAGS_calib_yaml.empty()) {
    // some mode requires FLAGS_calib_yaml
    if (FLAGS_depth) {
      LOG(ERROR) << "You must set calib_yaml to enable undistort";
      return -1;
    }
    if (FLAGS_calib_verify) {
      LOG(ERROR) << "You must set calib_yaml to enable calib_verify";
      return -1;
    }
    if (FLAGS_orb_verify) {
      LOG(ERROR) << "You must set calib_yaml to enable orb_verify";
      return -1;
    }
  }
  if (FLAGS_orb_verify && FLAGS_depth) {
    LOG(ERROR) << "orb_verify and undistort is set at the same time";
    return -1;
  }
  if (FLAGS_orb_verify && !FLAGS_record_path.empty()) {
    LOG(ERROR) << "orb_verify and record_path is set at the same time";
    return -1;
  }
  if (FLAGS_calib_verify && FLAGS_depth) {
    LOG(ERROR) << "calib_verify and undistort is set at the same time";
    return -1;
  }
  if (FLAGS_calib_verify && !FLAGS_record_path.empty()) {
    LOG(ERROR) << "calib_verify and record_path is set at the same time";
    return -1;
  }
  if (FLAGS_orb_verify && FLAGS_calib_verify) {
    LOG(ERROR) << "orb_verify and calib_verify is set at the same time";
    return -1;
  }
  if (!FLAGS_record_path.empty()) {
    // First make sure record_path exists
    namespace fs = boost::filesystem;
    fs::path rec_path(FLAGS_record_path);
    if (fs::create_directories(rec_path)) {
      cout << "Created " << FLAGS_record_path << "\n";
    }
    // If record_path already exists, we will append time at the end of
    // the record path and hopefully it will have no collision, except the special
    // case of spacebar_mode, as we may intend to continue saving images in the same
    // record path.
    fs::path imu_data_file(rec_path / "imu_data.txt");
    if (fs::is_regular_file(imu_data_file) && !FLAGS_spacebar_mode) {
      std::cout << "Found existing recording files at " << FLAGS_record_path << "\n";
      std::time_t t = std::time(NULL);
      char buf[32];
      std::strftime(buf, sizeof(buf), "_%H%M%S", std::localtime(&t));
      FLAGS_record_path += std::string(buf);
      std::cout << "Rename record path to " << FLAGS_record_path << "\n";
      rec_path = fs::path(FLAGS_record_path);
      if (fs::create_directories(rec_path)) {
        cout << "Created " << FLAGS_record_path << "\n";
      }
    }
  }
#ifndef HAS_OPENCV_VIZ
  if (FLAGS_viz3d) {
    LOG(ERROR) << "opencv viz3d is not installed. Turn off viz3d option";
    FLAGS_viz3d = false;
  }
#endif
  g_auto_gain = FLAGS_auto_gain;
  run_flag = true;
  g_auto_infrared = false;
  g_aec_index = 120;
  g_infrared_index = 125;

  // TODO(mingyu): Restore the support for XP3s?
  if (FLAGS_sensor_type == "LI"
      || FLAGS_sensor_type == "XP"
      || FLAGS_sensor_type == "XP2"
      || FLAGS_sensor_type == "XP3"
      || FLAGS_sensor_type == "XPIRL"
      || FLAGS_sensor_type == "XPIRL2"
      || FLAGS_sensor_type == "FACE") {
    g_xp_sensor_ptr.reset(new XpSensorMultithread(FLAGS_sensor_type,
                                                  g_auto_gain,
                                                  FLAGS_imu_from_image,
                                                  FLAGS_dev_id));
    if (g_xp_sensor_ptr->init(g_aec_index)) {
      VLOG(1) << "XpSensorMultithread init succeeeded!";
    } else {
      LOG(ERROR) << "XpSensorMultithread failed to init";
      return -1;
    }
    uint16_t width, height;
    // default param. Will be changed if calib yaml file is provided
    if (!(g_xp_sensor_ptr->get_sensor_resolution(&width, &height)))
      return -1;
    g_img_size.width = width;
    g_img_size.height = height;
    // FACE is a special XP3
    if (FLAGS_sensor_type == "FACE") {
      g_img_size.height = width;
      g_img_size.width = height;
    }
  } else {
    LOG(ERROR) << "sensor_type " << FLAGS_sensor_type << " not supported";
    return -1;
  }
  // Prepare the thread pool to handle the data from XpSensorMultithread
  vector<std::thread> thread_pool;
  thread_pool.push_back(std::thread(thread_proc_img));
  thread_pool.push_back(std::thread(thread_write_imu_data));
  if (!FLAGS_record_path.empty()) {
    thread_pool.push_back(std::thread(thread_save_img));
  }
  // Register callback functions and let XpSensorMultithread spin
  CHECK(g_xp_sensor_ptr);
  g_xp_sensor_ptr->set_image_data_callback(image_data_callback);
  g_xp_sensor_ptr->set_imu_data_callback(imu_data_callback);
  g_xp_sensor_ptr->run();
  for (auto& t : thread_pool) {
    t.join();
  }
  if (!g_xp_sensor_ptr->stop()) {
    LOG(ERROR) << "XpSensorMultithread failed to stop properly!";
  }
  cout << "finished safely" << std::endl;
  return 0;
}
