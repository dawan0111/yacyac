
#include "md/global.hpp"
#include "rclcpp/rclcpp.hpp"
#include "tf2/LinearMath/Quaternion.h"
#include <nav_msgs/msg/odometry.hpp>
#include <stdint.h>

#include <yacyac_interface/msg/pose.hpp>
#define VELOCITY_CONSTANT_VALUE                                                                                                                                                                        \
    9.5492743 // 이동속도(m/min), v = 바퀴 둘레의 길이 x RPM
              // 이동속도(m/sec), v = (2 x 바퀴 반지름 x (pi / 60) x RPM)
              // 0.10472 = (2 x pi / 60)
              // V = r * w = r * (RPM * 0.10472)
              //           = r * RPM * 0.10472
              // RPM = V / r * 9.5492743

#define constrain(amt, low, high) ((amt) <= (low) ? (low) : ((amt) >= (high) ? (high) : (amt)))

#define LEFT 0 // Swing direction
#define RIGHT 1

static double robot_old_x;
static double robot_old_y;
static double robot_old_theta;

extern nav_msgs::msg::Odometry robot_odom;
extern yacyac_interface::msg::Pose robot_pose;

void ResetOdom(void)
{
    robot_old_x = 0.0;
    robot_old_y = 0.0;
    robot_old_theta = 0.0;

    robot_pose.x = 0.0;
    robot_pose.y = 0.0;
    robot_pose.theta = 0.0;
    robot_pose.linear_velocity = 0.0;
    robot_pose.angular_velocity = 0.0;

    robot_odom.header.frame_id = "odom";
    robot_odom.child_frame_id = "base_link";
    robot_odom.pose.pose.position.x = 0.0;
    robot_odom.pose.pose.position.y = 0.0;
    robot_odom.pose.pose.position.z = 0.0;
    robot_odom.pose.pose.orientation.x = 0.0;
    robot_odom.pose.pose.orientation.y = 0.0;
    robot_odom.pose.pose.orientation.z = 0.0;
    robot_odom.pose.pose.orientation.w = 1.0;
}

// RPM --> m/sec
double* RPMSpeedToRobotSpeed(int16_t rpm_left, int16_t rpm_right)
{
    double v_left;
    double v_right;
    double temp;
    static double robot_speed[2];

    temp = (2.0 * M_PI * robotParamData.wheel_radius) / 60;

    v_left = temp * (double)rpm_left;
    v_right = temp * (double)rpm_right;

    robot_speed[0] = (v_right + v_left) / 2;
    robot_speed[1] = (v_right - v_left) / robotParamData.nWheelLength;

    return robot_speed;
}

// m/sec --> RPM
int16_t* RobotSpeedToRPMSpeed(double linear, double angular)
{
    double wheel_radius;
    double wheel_separation;
    double reduction;
    double wheel_velocity_cmd[2];
    static int16_t goal_rpm_spped[2];

    wheel_radius = robotParamData.wheel_radius;
    wheel_separation = robotParamData.nWheelLength;
    reduction = (double)robotParamData.nGearRatio;

    // ROS_INFO("l:%f, a:%f", (double)linear, (double)angular);

    wheel_velocity_cmd[LEFT] = linear - (angular * wheel_separation / 2);
    wheel_velocity_cmd[RIGHT] = linear + (angular * wheel_separation / 2);

    // ROS_INFO("left:%f, right:%f", (double)wheel_velocity_cmd[LEFT], (double)wheel_velocity_cmd[RIGHT]);

    //***************************************************************************************
    // Linearvelocity --> RPM 으로 환산
    //***************************************************************************************
    wheel_velocity_cmd[LEFT] = constrain(wheel_velocity_cmd[LEFT] * VELOCITY_CONSTANT_VALUE / wheel_radius * reduction, -robotParamData.nMaxRPM, robotParamData.nMaxRPM);
    wheel_velocity_cmd[RIGHT] = constrain(wheel_velocity_cmd[RIGHT] * VELOCITY_CONSTANT_VALUE / wheel_radius * reduction, -robotParamData.nMaxRPM, robotParamData.nMaxRPM);

    // ROS_INFO("RPM1 L:%f, R:%f\r\n", (double)wheel_velocity_cmd[LEFT], (double)wheel_velocity_cmd[RIGHT]);

    goal_rpm_spped[0] = (int16_t)(wheel_velocity_cmd[LEFT]);
    goal_rpm_spped[1] = (int16_t)(wheel_velocity_cmd[RIGHT]);

    return goal_rpm_spped;
}

void CalRobotPoseFromRPM(PID_PNT_MAIN_DATA_t* pData)
{
    static long previous_rpm;
    static rclcpp::Time previous_time;
    static int32_t old_mtr_pos_id1;
    static int32_t old_mtr_pos_id2;

    double lAvrRPM;
    double interval_time;
    int16_t rpm_left;
    int16_t rpm_right;
    double* pVelocity;
    double delta_s;
    double delta_theta;
    double robot_curr_x;
    double robot_curr_y;
    double robot_curr_theta;

    // auto curr_time = ros::Time::now();
    auto curr_time = rclcpp::Clock().now();

    //----------------------------------------------------------------
    rpm_left = pData->rpm_id1;
    if (rpm_left > 0) {
        rpm_left = rpm_left + (robotParamData.nGearRatio / 2);
    }
    else {
        rpm_left = rpm_left - (robotParamData.nGearRatio / 2);
    }
    rpm_left /= robotParamData.nGearRatio;

    //----------------------------------------------------------------
    rpm_right = pData->rpm_id2;
    if (rpm_right > 0) {
        rpm_right = rpm_right + (robotParamData.nGearRatio / 2);
    }
    else {
        rpm_right = rpm_right - (robotParamData.nGearRatio / 2);
    }
    rpm_right /= robotParamData.nGearRatio;

    //---------------------------------------------------------------------
    // RPM noise filter
    //---------------------------------------------------------------------
    lAvrRPM = (rpm_left + rpm_right) / 2;
    if (abs(lAvrRPM - previous_rpm) > (robotParamData.nMaxRPM / 2)) {
        lAvrRPM = previous_rpm;
    }
    previous_rpm = lAvrRPM;
    //---------------------------------------------------------------------

    pVelocity = RPMSpeedToRobotSpeed(rpm_left, rpm_right);

    interval_time = curr_time.seconds() - previous_time.seconds();
    previous_time = curr_time;

    delta_s = pVelocity[0] * interval_time;
    delta_theta = pVelocity[1] * interval_time;

    robot_curr_x = robot_old_x + delta_s * cos((long double)(robot_old_theta + (delta_theta / 2.0)));
    robot_curr_y = robot_old_y + delta_s * sin((long double)(robot_old_theta + (delta_theta / 2.0)));
    robot_curr_theta = robot_old_theta + delta_theta;

    robot_old_x = robot_curr_x;
    robot_old_y = robot_curr_y;
    robot_old_theta = robot_curr_theta;

    robot_pose.x = robot_curr_x;
    robot_pose.y = robot_curr_y;
    robot_pose.theta = robot_curr_theta;
    robot_pose.linear_velocity = pVelocity[0];
    robot_pose.angular_velocity = pVelocity[1];

    tf2::Quaternion quat;
    quat.setRPY(0.0, 0.0, robot_curr_theta);
    robot_odom.pose.pose.position.x = robot_curr_x;
    robot_odom.pose.pose.position.y = robot_curr_y;
    robot_odom.pose.pose.orientation.x = quat.x();
    robot_odom.pose.pose.orientation.y = quat.y();
    robot_odom.pose.pose.orientation.z = quat.z();
    robot_odom.pose.pose.orientation.w = quat.w();
    
    robot_odom.twist.twist.linear.x = pVelocity[0];
    robot_odom.twist.twist.linear.y = 0.0;
    robot_odom.twist.twist.angular.z = pVelocity[1];
}

void CalRobotPoseFromPos(PID_PNT_MAIN_DATA_t* pData)
{
    static bool first_cal = false;
    static long previous_rpm;
    static rclcpp::Time previous_time;
    static int32_t old_mtr_pos_id1;
    static int32_t old_mtr_pos_id2;

    double interval_time;
    double delta_s;
    double delta_theta;
    double robot_curr_x;
    double robot_curr_y;
    double robot_curr_theta;

    int32_t pos_left;
    int32_t pos_right;
    double v_left;
    double v_right;
    double vel_left;
    double vel_right;
    double linear_vel;
    double angular_vel;

    // ros::Time curr_time = ros::Time::now();
    auto curr_time = rclcpp::Clock().now();

    pos_left = pData->mtr_pos_id1;
    pos_right = pData->mtr_pos_id2;

    if (first_cal == false) {
        first_cal = true;

        old_mtr_pos_id1 = pos_left;
        old_mtr_pos_id2 = pos_right;

        robot_pose.x = 0;
        robot_pose.y = 0;
        robot_pose.theta = 0;
        robot_pose.linear_velocity = 0;
        robot_pose.angular_velocity = 0;

        return;
    }

    interval_time = curr_time.seconds() - previous_time.seconds();
    previous_time = curr_time;

    v_left = (double)(pos_left - old_mtr_pos_id1);
    v_right = (double)(pos_right - old_mtr_pos_id2);

    v_left = v_left * robotParamData.motor_count_per_degree;
    v_right = v_right * robotParamData.motor_count_per_degree;

    v_left = (v_left / interval_time) * M_PI / 180.0;
    v_right = (v_right / interval_time) * M_PI / 180.0;

    old_mtr_pos_id1 = pos_left;
    old_mtr_pos_id2 = pos_right;

    vel_left = v_left * robotParamData.wheel_radius;
    vel_right = v_right * robotParamData.wheel_radius;

    linear_vel = (vel_right + vel_left) / 2;
    angular_vel = (vel_right - vel_left) / robotParamData.nWheelLength;

    delta_s = linear_vel * interval_time;
    delta_theta = angular_vel * interval_time;

    robot_curr_x = robot_old_x + delta_s * cos((long double)(robot_old_theta + (delta_theta / 2.0)));
    robot_curr_y = robot_old_y + delta_s * sin((long double)(robot_old_theta + (delta_theta / 2.0)));
    robot_curr_theta = robot_old_theta + delta_theta;

    robot_old_x = robot_curr_x;
    robot_old_y = robot_curr_y;
    robot_old_theta = robot_curr_theta;

    robot_pose.x = robot_curr_x;
    robot_pose.y = robot_curr_y;
    robot_pose.theta = robot_curr_theta;
    robot_pose.linear_velocity = linear_vel;
    robot_pose.angular_velocity = angular_vel;

    tf2::Quaternion quat;
    quat.setRPY(0.0, 0.0, robot_curr_theta);
    robot_odom.pose.pose.position.x = robot_curr_x;
    robot_odom.pose.pose.position.y = robot_curr_y;
    robot_odom.pose.pose.orientation.x = quat.x();
    robot_odom.pose.pose.orientation.y = quat.y();
    robot_odom.pose.pose.orientation.z = quat.z();
    robot_odom.pose.pose.orientation.w = quat.w();
    
    robot_odom.twist.twist.linear.x = linear_vel;
    robot_odom.twist.twist.linear.y = 0.0;
    robot_odom.twist.twist.angular.z = angular_vel;
}

void CalRobotPose_old(PID_PNT_MAIN_DATA_t* pData)
{
    static long previous_rpm;
    static rclcpp::Time previous_time;

    double lAvrRPM;
    double interval_time;
    int16_t rpm_left;
    int16_t rpm_right;
    double* pVelocity;
    double delta_s;
    double delta_theta;
    double robot_curr_x;
    double robot_curr_y;
    double robot_curr_theta;

    // ros::Time curr_time = ros::Time::now();
    auto curr_time = rclcpp::Clock().now();

    //----------------------------------------------------------------
    rpm_left = pData->rpm_id1;
    // if(robotParamData.mtr_1_dir_state == 1) {
    //     rpm_left *= -1;
    // }

    if (rpm_left > 0) {
        rpm_left = rpm_left + (robotParamData.nGearRatio / 2);
    }
    else {
        rpm_left = rpm_left - (robotParamData.nGearRatio / 2);
    }
    rpm_left /= robotParamData.nGearRatio;

    //----------------------------------------------------------------
    rpm_right = pData->rpm_id2;
    // if(robotParamData.mtr_2_dir_state == 1) {
    //     rpm_right *= -1;
    // }

    if (rpm_right > 0) {
        rpm_right = rpm_right + (robotParamData.nGearRatio / 2);
    }
    else {
        rpm_right = rpm_right - (robotParamData.nGearRatio / 2);
    }
    rpm_right /= robotParamData.nGearRatio;

    //---------------------------------------------------------------------
    // RPM noise filter
    //---------------------------------------------------------------------
    lAvrRPM = (rpm_left + rpm_right) / 2;
    if (abs(lAvrRPM - previous_rpm) > (robotParamData.nMaxRPM / 2)) {
        lAvrRPM = previous_rpm;
    }
    previous_rpm = lAvrRPM;
    //---------------------------------------------------------------------

    pVelocity = RPMSpeedToRobotSpeed(rpm_left, rpm_right);

    interval_time = curr_time.seconds() - previous_time.seconds();
    previous_time = curr_time;

    delta_s = pVelocity[0] * interval_time;
    delta_theta = pVelocity[1] * interval_time;

    robot_curr_x = robot_old_x + delta_s * cos((long double)(robot_old_theta + (delta_theta / 2.0)));
    robot_curr_y = robot_old_y + delta_s * sin((long double)(robot_old_theta + (delta_theta / 2.0)));
    robot_curr_theta = robot_old_theta + delta_theta;

    robot_old_x = robot_curr_x;
    robot_old_y = robot_curr_y;
    robot_old_theta = robot_curr_theta;

    robot_pose.x = robot_curr_x;
    robot_pose.y = robot_curr_y;
    robot_pose.theta = robot_curr_theta;
    robot_pose.linear_velocity = pVelocity[0];
    robot_pose.angular_velocity = pVelocity[1];
    tf2::Quaternion quat;
    quat.setRPY(0.0, 0.0, robot_curr_theta);
    robot_odom.pose.pose.position.x = robot_curr_x;
    robot_odom.pose.pose.position.y = robot_curr_y;
    robot_odom.pose.pose.orientation.x = quat.x();
    robot_odom.pose.pose.orientation.y = quat.y();
    robot_odom.pose.pose.orientation.z = quat.z();
    robot_odom.pose.pose.orientation.w = quat.w();
    
    robot_odom.twist.twist.linear.x = pVelocity[0];
    robot_odom.twist.twist.linear.y = 0.0;
    robot_odom.twist.twist.angular.z = pVelocity[1];
}
