#include "selfdrive/ui/ui.h"

#include <unistd.h>
#include <string>  //opkr
#include <iostream>

#include <cassert>
#include <cmath>
#include <cstdio>

#include "selfdrive/common/util.h"
#include "selfdrive/common/watchdog.h"
#include "selfdrive/hardware/hw.h"

#define BACKLIGHT_DT 0.05
#define BACKLIGHT_TS 10.00
#define BACKLIGHT_OFFROAD 75


// Projects a point in car to space to the corresponding point in full frame
// image space.
static bool calib_frame_to_full_frame(const UIState *s, float in_x, float in_y, float in_z, vertex_data *out) {
  const float margin = 500.0f;
  const vec3 pt = (vec3){{in_x, in_y, in_z}};
  const vec3 Ep = matvecmul3(s->scene.view_from_calib, pt);
  const vec3 KEp = matvecmul3(s->wide_camera ? ecam_intrinsic_matrix : fcam_intrinsic_matrix, Ep);

  // Project.
  float x = KEp.v[0] / KEp.v[2];
  float y = KEp.v[1] / KEp.v[2];

  nvgTransformPoint(&out->x, &out->y, s->car_space_transform, x, y);
  return out->x >= -margin && out->x <= s->fb_w + margin && out->y >= -margin && out->y <= s->fb_h + margin;
}

static int get_path_length_idx(const cereal::ModelDataV2::XYZTData::Reader &line, const float path_height) {
  const auto line_x = line.getX();
  int max_idx = 0;
  for (int i = 0; i < TRAJECTORY_SIZE && line_x[i] < path_height; ++i) {
    max_idx = i;
  }
  return max_idx;
}

static void update_leads(UIState *s, const cereal::ModelDataV2::Reader &model) {
  auto leads = model.getLeadsV3();
  auto model_position = model.getPosition();
  for (int i = 0; i < 2; ++i) {
    if (leads[i].getProb() > 0.5) {
      float z = model_position.getZ()[get_path_length_idx(model_position, leads[i].getX()[0])];
      calib_frame_to_full_frame(s, leads[i].getX()[0], leads[i].getY()[0], z + 1.22, &s->scene.lead_vertices[i]);
    }
  }
}

static void update_line_data(const UIState *s, const cereal::ModelDataV2::XYZTData::Reader &line,
                             float y_off, float z_off, line_vertices_data *pvd, int max_idx) {
  const auto line_x = line.getX(), line_y = line.getY(), line_z = line.getZ();
  vertex_data *v = &pvd->v[0];
  for (int i = 0; i <= max_idx; i++) {
    v += calib_frame_to_full_frame(s, line_x[i], line_y[i] - y_off, line_z[i] + z_off, v);
  }
  for (int i = max_idx; i >= 0; i--) {
    v += calib_frame_to_full_frame(s, line_x[i], line_y[i] + y_off, line_z[i] + z_off, v);
  }
  pvd->cnt = v - pvd->v;
  assert(pvd->cnt <= std::size(pvd->v));
}

static void update_model(UIState *s, const cereal::ModelDataV2::Reader &model) {
  UIScene &scene = s->scene;
  auto model_position = model.getPosition();
  float max_distance = std::clamp(model_position.getX()[TRAJECTORY_SIZE - 1],
                                  MIN_DRAW_DISTANCE, MAX_DRAW_DISTANCE);

  // update lane lines
  const auto lane_lines = model.getLaneLines();
  const auto lane_line_probs = model.getLaneLineProbs();
  int max_idx = get_path_length_idx(lane_lines[0], max_distance);
  for (int i = 0; i < std::size(scene.lane_line_vertices); i++) {
    scene.lane_line_probs[i] = lane_line_probs[i];
    update_line_data(s, lane_lines[i], 0.025 * scene.lane_line_probs[i], 0, &scene.lane_line_vertices[i], max_idx);
  }

  // update road edges
  const auto road_edges = model.getRoadEdges();
  const auto road_edge_stds = model.getRoadEdgeStds();
  for (int i = 0; i < std::size(scene.road_edge_vertices); i++) {
    scene.road_edge_stds[i] = road_edge_stds[i];
    update_line_data(s, road_edges[i], 0.025, 0, &scene.road_edge_vertices[i], max_idx);
  }

  // update path
  auto lead_one = model.getLeadsV3()[0];
  if (lead_one.getProb() > 0.5) {
    const float lead_d = lead_one.getX()[0] * 2.;
    max_distance = std::clamp((float)(lead_d - fmin(lead_d * 0.35, 10.)), 0.0f, max_distance);
  }
  max_idx = get_path_length_idx(model_position, max_distance);
  update_line_data(s, model_position, 0.5, 1.22, &scene.track_vertices, max_idx);
}

static void update_sockets(UIState *s) {
  s->sm->update(0);
}

static void update_state(UIState *s) {
  SubMaster &sm = *(s->sm);
  UIScene &scene = s->scene;

  // update engageability and DM icons at 2Hz
  if (sm.frame % (UI_FREQ / 2) == 0) {
    scene.engageable = sm["controlsState"].getControlsState().getEngageable();
    scene.dm_active = sm["driverMonitoringState"].getDriverMonitoringState().getIsActiveMode();
  }

  if (sm.updated("controlsState")) {
    scene.controls_state = sm["controlsState"].getControlsState();
    scene.lateralControlMethod = scene.controls_state.getLateralControlMethod();
    if (scene.lateralControlMethod == 0) {
      scene.output_scale = scene.controls_state.getLateralControlState().getPidState().getOutput();
    } else if (scene.lateralControlMethod == 1) {
      scene.output_scale = scene.controls_state.getLateralControlState().getIndiState().getOutput();
    } else if (scene.lateralControlMethod == 2) {
      scene.output_scale = scene.controls_state.getLateralControlState().getLqrState().getOutput();
    }

    scene.alertTextMsg1 = scene.controls_state.getAlertTextMsg1(); //debug1
    scene.alertTextMsg2 = scene.controls_state.getAlertTextMsg2(); //debug2

    scene.limitSpeedCamera = scene.controls_state.getLimitSpeedCamera();
    scene.limitSpeedCameraDist = scene.controls_state.getLimitSpeedCameraDist();
    scene.mapSign = scene.controls_state.getMapSign();
    scene.steerRatio = scene.controls_state.getSteerRatio();
    scene.dynamic_tr_mode = scene.controls_state.getDynamicTRMode();
    scene.dynamic_tr_value = scene.controls_state.getDynamicTRValue();
  }
  if (sm.updated("carState")) {
    scene.car_state = sm["carState"].getCarState();
    auto cs_data = sm["carState"].getCarState();
    auto cruiseState = scene.car_state.getCruiseState();
    scene.awake = cruiseState.getCruiseSwState();

    if (scene.leftBlinker!=cs_data.getLeftBlinker() || scene.rightBlinker!=cs_data.getRightBlinker()) {
      scene.blinker_blinkingrate = 120;
    }
    scene.brakePress = cs_data.getBrakePressed();
    scene.brakeLights = cs_data.getBrakeLights();
    scene.getGearShifter = cs_data.getGearShifter();
    scene.leftBlinker = cs_data.getLeftBlinker();
    scene.rightBlinker = cs_data.getRightBlinker();
    scene.leftblindspot = cs_data.getLeftBlindspot();
    scene.rightblindspot = cs_data.getRightBlindspot();
    scene.tpmsPressureFl = cs_data.getTpmsPressureFl();
    scene.tpmsPressureFr = cs_data.getTpmsPressureFr();
    scene.tpmsPressureRl = cs_data.getTpmsPressureRl();
    scene.tpmsPressureRr = cs_data.getTpmsPressureRr();
    scene.radarDistance = cs_data.getRadarDistance();
    scene.standStill = cs_data.getStandStill();
    scene.vSetDis = cs_data.getVSetDis();
    scene.cruiseAccStatus = cs_data.getCruiseAccStatus();
    scene.angleSteers = cs_data.getSteeringAngleDeg();
    scene.cruise_gap = cs_data.getCruiseGapSet();
    scene.brakeHold = cs_data.getBrakeHold();
    scene.steer_warning = cs_data.getSteerWarning();
    scene.stand_still = cs_data.getStandstill();
  }

  if (sm.updated("liveParameters")) {
    //scene.liveParams = sm["liveParameters"].getLiveParameters();
    auto live_data = sm["liveParameters"].getLiveParameters();
    scene.liveParams.angleOffset = live_data.getAngleOffsetDeg();
    scene.liveParams.angleOffsetAverage = live_data.getAngleOffsetAverageDeg();
    scene.liveParams.stiffnessFactor = live_data.getStiffnessFactor();
    scene.liveParams.steerRatio = live_data.getSteerRatio();
  }
  if (sm.updated("modelV2") && s->vg) {
    auto model = sm["modelV2"].getModelV2();
    update_model(s, model);
    update_leads(s, model);
  }
  if (sm.updated("liveCalibration")) {
    scene.world_objects_visible = true;
    auto rpy_list = sm["liveCalibration"].getLiveCalibration().getRpyCalib();
    Eigen::Vector3d rpy;
    rpy << rpy_list[0], rpy_list[1], rpy_list[2];
    Eigen::Matrix3d device_from_calib = euler2rot(rpy);
    Eigen::Matrix3d view_from_device;
    view_from_device << 0,1,0,
                        0,0,1,
                        1,0,0;
    Eigen::Matrix3d view_from_calib = view_from_device * device_from_calib;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        scene.view_from_calib.v[i*3 + j] = view_from_calib(i,j);
      }
    }
  }
  if (sm.updated("deviceState")) {
    scene.deviceState = sm["deviceState"].getDeviceState();
    scene.cpuPerc = (scene.deviceState.getCpuUsagePercent()[0] + scene.deviceState.getCpuUsagePercent()[1] + scene.deviceState.getCpuUsagePercent()[2] + scene.deviceState.getCpuUsagePercent()[3])/4;
    scene.cpuTemp = (scene.deviceState.getCpuTempC()[0] + scene.deviceState.getCpuTempC()[1] + scene.deviceState.getCpuTempC()[2] + scene.deviceState.getCpuTempC()[3])/4;
    scene.batTemp = scene.deviceState.getBatteryTempC();
    scene.ambientTemp = scene.deviceState.getAmbientTempC();
    scene.fanSpeed = scene.deviceState.getFanSpeedPercentDesired();
    scene.batPercent = scene.deviceState.getBatteryPercent();
  }
  if (sm.updated("pandaState")) {
    auto pandaState = sm["pandaState"].getPandaState();
    scene.pandaType = pandaState.getPandaType();
    scene.ignition = pandaState.getIgnitionLine() || pandaState.getIgnitionCan();
    scene.controlAllowed = pandaState.getControlsAllowed();
  } else if ((s->sm->frame - s->sm->rcv_frame("pandaState")) > 5*UI_FREQ) {
    scene.pandaType = cereal::PandaState::PandaType::UNKNOWN;
  }
  if (sm.updated("ubloxGnss")) {
    auto ub_data = sm["ubloxGnss"].getUbloxGnss();
    if (ub_data.which() == cereal::UbloxGnss::MEASUREMENT_REPORT) {
      scene.satelliteCount = ub_data.getMeasurementReport().getNumMeas();
    }
  }
  if (sm.updated("gpsLocationExternal")) {
    scene.gpsAccuracy = sm["gpsLocationExternal"].getGpsLocationExternal().getAccuracy();
    auto ge_data = sm["gpsLocationExternal"].getGpsLocationExternal();
    scene.gpsAccuracyUblox = ge_data.getAccuracy();
    scene.altitudeUblox = ge_data.getAltitude();
    scene.bearingUblox = ge_data.getBearingDeg();
  }
  if (sm.updated("carParams")) {
    scene.longitudinal_control = sm["carParams"].getCarParams().getOpenpilotLongitudinalControl();
    scene.steerMax_V = sm["carParams"].getCarParams().getSteerMaxV()[0];
    scene.steer_actuator_delay = sm["carParams"].getCarParams().getSteerActuatorDelay();
  }
  if (sm.updated("lateralPlan")) {
    scene.lateral_plan = sm["lateralPlan"].getLateralPlan();
    auto lp_data = sm["lateralPlan"].getLateralPlan();
    scene.lateralPlan.laneWidth = lp_data.getLaneWidth();
    scene.lateralPlan.dProb = lp_data.getDProb();
    scene.lateralPlan.lProb = lp_data.getLProb();
    scene.lateralPlan.rProb = lp_data.getRProb();
    scene.lateralPlan.steerRateCost = lp_data.getSteerRateCost();
    scene.lateralPlan.standstillElapsedTime = lp_data.getStandstillElapsedTime();
    scene.lateralPlan.lanelessModeStatus = lp_data.getLanelessMode();
  }
  // opkr
  if (sm.updated("liveMapData")) {
    scene.live_map_data = sm["liveMapData"].getLiveMapData();
    auto lm_data = sm["liveMapData"].getLiveMapData();
    scene.liveMapData.opkrspeedlimit = lm_data.getSpeedLimit();
    scene.liveMapData.opkrspeedlimitdist = lm_data.getSpeedLimitDistance();
    scene.liveMapData.opkrspeedsign = lm_data.getSafetySign();
    scene.liveMapData.opkrcurveangle = lm_data.getRoadCurvature();
    scene.liveMapData.opkrturninfo = lm_data.getTurnInfo();
    scene.liveMapData.opkrdisttoturn = lm_data.getDistanceToTurn();
  }
  if (sm.updated("sensorEvents")) {
    for (auto sensor : sm["sensorEvents"].getSensorEvents()) {
      if (!scene.started && sensor.which() == cereal::SensorEventData::ACCELERATION) {
        auto accel = sensor.getAcceleration().getV();
        if (accel.totalSize().wordCount) { // TODO: sometimes empty lists are received. Figure out why
          scene.accel_sensor = accel[2];
        }
      } else if (!scene.started && sensor.which() == cereal::SensorEventData::GYRO_UNCALIBRATED) {
        auto gyro = sensor.getGyroUncalibrated().getV();
        if (gyro.totalSize().wordCount) {
          scene.gyro_sensor = gyro[1];
        }
      }
    }
  }
  if (sm.updated("roadCameraState")) {
    auto camera_state = sm["roadCameraState"].getRoadCameraState();

    float max_lines = Hardware::EON() ? 5408 : 1904;
    float max_gain = Hardware::EON() ? 1.0: 10.0;
    float max_ev = max_lines * max_gain;

    if (Hardware::TICI) {
      max_ev /= 6;
    }

    float ev = camera_state.getGain() * float(camera_state.getIntegLines());

    scene.light_sensor = std::clamp<float>(1.0 - (ev / max_ev), 0.0, 1.0);
  }
  if (!scene.is_OpenpilotViewEnabled) {
    scene.started = sm["deviceState"].getDeviceState().getStarted() && scene.ignition;
  } else {
    scene.started = sm["deviceState"].getDeviceState().getStarted();
  }
}

static void update_params(UIState *s) {
  const uint64_t frame = s->sm->frame;
  UIScene &scene = s->scene;
  Params params;
  if (frame % (5*UI_FREQ) == 0) {
    scene.is_metric = params.getBool("IsMetric");
    scene.is_OpenpilotViewEnabled = params.getBool("IsOpenpilotViewEnabled");
  }
  //opkr navi on boot
  if (!scene.navi_on_boot && (frame - scene.started_frame > 5*UI_FREQ)) {
    if (params.getBool("OpkrRunNaviOnBoot") && params.getBool("ControlsReady") && (params.get("CarParams").size() > 0)) {
      scene.navi_on_boot = true;
      scene.map_is_running = true;
      scene.map_on_top = true;
      scene.map_on_overlay = false;
      params.putBool("OpkrMapEnable", true);
      system("am start com.mnsoft.mappyobn/com.mnsoft.mappy.MainActivity");
    } else if (frame - scene.started_frame > 20*UI_FREQ) {
      scene.navi_on_boot = true;
    }
  }
  if (!scene.move_to_background && (frame - scene.started_frame > 10*UI_FREQ)) {
    if (params.getBool("OpkrRunNaviOnBoot") && params.getBool("OpkrMapEnable") && params.getBool("ControlsReady") && (params.get("CarParams").size() > 0)) {
      scene.move_to_background = true;
      scene.map_on_top = false;
      scene.map_on_overlay = true;
      system("am start --activity-task-on-home com.opkr.maphack/com.opkr.maphack.MainActivity");
    } else if (frame - scene.started_frame > 20*UI_FREQ) {
      scene.move_to_background = true;
    }
  }
  if (!scene.auto_gitpull && (frame - scene.started_frame > 15*UI_FREQ)) {
    if (params.getBool("GitPullOnBoot")) {
      scene.auto_gitpull = true;
      system("/data/openpilot/selfdrive/assets/addon/script/gitpull.sh");
    } else if (frame - scene.started_frame > 20*UI_FREQ) {
      scene.auto_gitpull = true;
    }
  }

  if (!scene.read_params_once) {
    // user param value init
    scene.end_to_end = params.getBool("EndToEndToggle");
    scene.driving_record = params.getBool("OpkrDrivingRecord");
    scene.nDebugUi1 = params.getBool("DebugUi1");
    scene.nDebugUi2 = params.getBool("DebugUi2");
    scene.forceGearD = params.getBool("JustDoGearD");
    scene.nOpkrBlindSpotDetect = params.getBool("OpkrBlindSpotDetect");
    scene.laneless_mode = std::stoi(params.get("LanelessMode"));
    scene.recording_count = std::stoi(params.get("RecordingCount"));
    scene.recording_quality = std::stoi(params.get("RecordingQuality"));
    scene.speed_lim_off = std::stoi(params.get("OpkrSpeedLimitOffset"));
    scene.monitoring_mode = params.getBool("OpkrMonitoringMode");
    scene.brightness = std::stoi(params.get("OpkrUIBrightness"));
    scene.nVolumeBoost = std::stoi(params.get("OpkrUIVolumeBoost"));
    scene.autoScreenOff = std::stoi(params.get("OpkrAutoScreenOff"));
    scene.brightness_off = std::stoi(params.get("OpkrUIBrightnessOff"));
    scene.cameraOffset = std::stoi(params.get("CameraOffsetAdj"));
    scene.pathOffset = std::stoi(params.get("PathOffsetAdj"));
    scene.osteerRateCost = std::stoi(params.get("SteerRateCostAdj"));
    scene.pidKp = std::stoi(params.get("PidKp"));
    scene.pidKi = std::stoi(params.get("PidKi"));
    scene.pidKd = std::stoi(params.get("PidKd"));
    scene.pidKf = std::stoi(params.get("PidKf"));
    scene.indiInnerLoopGain = std::stoi(params.get("InnerLoopGain"));
    scene.indiOuterLoopGain = std::stoi(params.get("OuterLoopGain"));
    scene.indiTimeConstant = std::stoi(params.get("TimeConstant"));
    scene.indiActuatorEffectiveness = std::stoi(params.get("ActuatorEffectiveness"));
    scene.lqrScale = std::stoi(params.get("Scale"));
    scene.lqrKi = std::stoi(params.get("LqrKi"));
    scene.lqrDcGain = std::stoi(params.get("DcGain"));
    scene.live_tune_panel_enable = params.getBool("OpkrLiveTunePanelEnable");
    scene.kr_date_show = params.getBool("KRDateShow");
    scene.kr_time_show = params.getBool("KRTimeShow");
    scene.steer_wind_down = params.getBool("SteerWindDown");
    scene.show_error = params.getBool("ShowError");

    if (scene.autoScreenOff > 0) {
      scene.nTime = scene.autoScreenOff * 60 * UI_FREQ;
    } else if (scene.autoScreenOff == 0) {
      scene.nTime = 30 * UI_FREQ;
    } else if (scene.autoScreenOff == -1) {
      scene.nTime = 15 * UI_FREQ;
    } else {
      scene.nTime = -1;
    }
    scene.comma_stock_ui = params.getBool("CommaStockUI");
    scene.opkr_livetune_ui = params.getBool("OpkrLiveTunePanelEnable");
    scene.batt_less = params.getBool("OpkrBattLess");
    scene.read_params_once = true;
  }
}

static void update_status(UIState *s) {
  if (s->scene.started && s->sm->updated("controlsState")) {
    auto controls_state = (*s->sm)["controlsState"].getControlsState();
    auto alert_status = controls_state.getAlertStatus();
    if (alert_status == cereal::ControlsState::AlertStatus::USER_PROMPT) {
      s->status = STATUS_WARNING;
    } else if (alert_status == cereal::ControlsState::AlertStatus::CRITICAL) {
      s->status = STATUS_ALERT;
    } else {
      s->status = controls_state.getEnabled() ? STATUS_ENGAGED : STATUS_DISENGAGED;
    }
  }

  // Handle onroad/offroad transition
  static bool started_prev = false;
  if (s->scene.started != started_prev) {
    if (s->scene.started) {
      s->status = STATUS_DISENGAGED;
      s->scene.started_frame = s->sm->frame;
      s->wide_camera = Hardware::TICI() ? Params().getBool("EnableWideCamera") : false;
    }
    // Invisible until we receive a calibration message.
    s->scene.world_objects_visible = false;
  }
  started_prev = s->scene.started;
}


QUIState::QUIState(QObject *parent) : QObject(parent) {
  ui_state.sm = std::make_unique<SubMaster, const std::initializer_list<const char *>>({
    "modelV2", "controlsState", "liveCalibration", "deviceState", "roadCameraState",
    "pandaState", "carParams", "driverMonitoringState", "sensorEvents", "carState", "liveLocationKalman",
    "ubloxGnss", "gpsLocationExternal", "liveParameters", "lateralPlan", "liveMapData",
  });

  ui_state.wide_camera = Hardware::TICI() ? Params().getBool("EnableWideCamera") : false;
  ui_state.sidebar_view = false;

  // update timer
  timer = new QTimer(this);
  QObject::connect(timer, &QTimer::timeout, this, &QUIState::update);
  timer->start(1000 / UI_FREQ);
}

void QUIState::update() {
  update_params(&ui_state);
  update_sockets(&ui_state);
  update_state(&ui_state);
  update_status(&ui_state);

  if (ui_state.scene.started != started_prev || ui_state.sm->frame == 1) {
    started_prev = ui_state.scene.started;
    emit offroadTransition(!ui_state.scene.started);
  }

  watchdog_kick();
  emit uiUpdate(ui_state);
}

Device::Device(QObject *parent) : brightness_filter(BACKLIGHT_OFFROAD, BACKLIGHT_TS, BACKLIGHT_DT), QObject(parent) {
}

void Device::update(const UIState &s) {
  updateBrightness(s);
  updateWakefulness(s);

  // TODO: remove from UIState and use signals
  QUIState::ui_state.awake = awake;
}

void Device::setAwake(bool on, bool reset) {
  if (on != awake) {
    awake = on;
    Hardware::set_display_power(awake);
    LOGD("setting display power %d", awake);
    emit displayPowerChanged(awake);
  }

  if (reset) {
    awake_timeout = 30 * UI_FREQ;
  }
}

void Device::updateBrightness(const UIState &s) {
  // Scale to 0% to 100%
  float clipped_brightness = 100.0 * s.scene.light_sensor;

  // CIE 1931 - https://www.photonstophotos.net/GeneralTopics/Exposure/Psychometric_Lightness_and_Gamma.htm
  if (clipped_brightness <= 8) {
    clipped_brightness = (clipped_brightness / 903.3);
  } else {
    clipped_brightness = std::pow((clipped_brightness + 16.0) / 116.0, 3.0);
  }

  // Scale back to 10% to 100%
  clipped_brightness = std::clamp(100.0f * clipped_brightness, 10.0f, 100.0f);

  if (!s.scene.started) {
    clipped_brightness = BACKLIGHT_OFFROAD;
  } else if (s.scene.autoScreenOff != -2 && s.scene.touched2) {
    sleep_time = s.scene.nTime;
  } else if (s.scene.controls_state.getAlertSize() != cereal::ControlsState::AlertSize::NONE && s.scene.autoScreenOff != -2) {
    sleep_time = s.scene.nTime;
  } else if (sleep_time > 0 && s.scene.autoScreenOff != -2) {
    sleep_time--;
  } else if (s.scene.started && sleep_time == -1 && s.scene.autoScreenOff != -2) {
    sleep_time = s.scene.nTime;
  }

  int brightness = brightness_filter.update(clipped_brightness);
  if (!awake) {
    brightness = 0;
  } else if (s.scene.started && sleep_time == 0 && s.scene.autoScreenOff != -2) {
    brightness = s.scene.brightness_off * 0.01 * brightness;
  } else if( s.scene.brightness ) {
    brightness = s.scene.brightness * 0.99;
  }

  if (brightness != last_brightness) {
    std::thread{Hardware::set_brightness, brightness}.detach();
  }
  last_brightness = brightness;
}

void Device::updateWakefulness(const UIState &s) {
  awake_timeout = std::max(awake_timeout - 1, 0);

  bool should_wake = s.scene.started || s.scene.ignition;
  if (!should_wake) {
    // tap detection while display is off
    bool accel_trigger = abs(s.scene.accel_sensor - accel_prev) > 0.2;
    bool gyro_trigger = abs(s.scene.gyro_sensor - gyro_prev) > 0.15;
    should_wake = accel_trigger && gyro_trigger;
    gyro_prev = s.scene.gyro_sensor;
    accel_prev = (accel_prev * (accel_samples - 1) + s.scene.accel_sensor) / accel_samples;
  }

  setAwake(awake_timeout, should_wake);
}
