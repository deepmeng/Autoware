/*
 *  Copyright (c) 2017, Tier IV, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <thread>
#include <chrono>
#include <map>

#include <ros/ros.h>
#include <std_msgs/Bool.h>
#include <std_msgs/String.h>
#include <geometry_msgs/TwistStamped.h>

#include "autoware_msgs/RemoteCmd.h"
#include "autoware_msgs/TwistGate.h"
#include "tablet_socket_msgs/mode_cmd.h"
#include "tablet_socket_msgs/gear_cmd.h"
#include "autoware_msgs/accel_cmd.h"
#include "autoware_msgs/brake_cmd.h"
#include "autoware_msgs/steer_cmd.h"
#include "autoware_msgs/ControlCommandStamped.h"

class TwistGate
{
  using remote_msgs_t = autoware_msgs::RemoteCmd;
  using twist_gate_msgs_t = autoware_msgs::TwistGate;

  public:
    TwistGate(const ros::NodeHandle& nh, const ros::NodeHandle& private_nh);
    ~TwistGate();
  private:
    void watchdog_timer();
    void remote_cmd_callback(const remote_msgs_t::ConstPtr& input_msg);
    void auto_cmd_twist_cmd_callback(const geometry_msgs::TwistStamped::ConstPtr& input_msg);
    void auto_cmd_mode_cmd_callback(const tablet_socket_msgs::mode_cmd::ConstPtr& input_msg);
    void auto_cmd_gear_cmd_callback(const tablet_socket_msgs::gear_cmd::ConstPtr& input_msg);
    void auto_cmd_accel_cmd_callback(const autoware_msgs::accel_cmd::ConstPtr& input_msg);
    void auto_cmd_steer_cmd_callback(const autoware_msgs::steer_cmd::ConstPtr& input_msg);
    void auto_cmd_brake_cmd_callback(const autoware_msgs::brake_cmd::ConstPtr& input_msg);
    void auto_cmd_ctrl_cmd_callback(const autoware_msgs::ControlCommandStamped::ConstPtr& input_msg);

    void reset_vehicle_cmd_msg();

    ros::NodeHandle nh_;
    ros::NodeHandle private_nh_;
    ros::Publisher emergency_stop_pub_;
    ros::Publisher control_command_pub_;
    ros::Publisher vehicle_cmd_pub_;
    ros::Subscriber remote_cmd_sub_;
    std::map<std::string , ros::Subscriber> auto_cmd_sub_stdmap_;

    twist_gate_msgs_t twist_gate_msg_;
    std_msgs::Bool emergency_stop_msg_;
    ros::Time remote_cmd_time_;
    ros::Duration timeout_period_;

    std::thread watchdog_timer_thread_;
    enum class CommandMode{AUTO=1, REMOTE=2} command_mode_, previous_command_mode_;
    std_msgs::String command_mode_topic_;
};

TwistGate::TwistGate(const ros::NodeHandle& nh, const ros::NodeHandle& private_nh) :
     nh_(nh)
    ,private_nh_(private_nh)
    ,timeout_period_(1.0)
    ,command_mode_(CommandMode::AUTO)
    ,previous_command_mode_(CommandMode::AUTO)
{
  emergency_stop_pub_ = nh_.advertise<std_msgs::Bool>("/emergency_stop", 1, true);
  control_command_pub_ = nh_.advertise<std_msgs::String>("/ctrl_mode", 1);
  vehicle_cmd_pub_ = nh_.advertise<twist_gate_msgs_t>("/vehicle_cmd", 1, true);

  remote_cmd_sub_ = nh_.subscribe("/remote_cmd", 1, &TwistGate::remote_cmd_callback, this);

  auto_cmd_sub_stdmap_["twist_cmd"] = nh_.subscribe("/twist_cmd", 1, &TwistGate::auto_cmd_twist_cmd_callback, this);
  auto_cmd_sub_stdmap_["mode_cmd"] = nh_.subscribe("/mode_cmd", 1, &TwistGate::auto_cmd_mode_cmd_callback, this);
  auto_cmd_sub_stdmap_["gear_cmd"] = nh_.subscribe("/gear_cmd", 1, &TwistGate::auto_cmd_gear_cmd_callback, this);
  auto_cmd_sub_stdmap_["accel_cmd"] = nh_.subscribe("/accel_cmd", 1, &TwistGate::auto_cmd_accel_cmd_callback, this);
  auto_cmd_sub_stdmap_["steer_cmd"] = nh_.subscribe("/steer_cmd", 1, &TwistGate::auto_cmd_steer_cmd_callback, this);
  auto_cmd_sub_stdmap_["brake_cmd"] = nh_.subscribe("/brake_cmd", 1, &TwistGate::auto_cmd_brake_cmd_callback, this);
  auto_cmd_sub_stdmap_["ctrl_cmd"] = nh_.subscribe("/ctrl_cmd", 1, &TwistGate::auto_cmd_ctrl_cmd_callback, this);

  twist_gate_msg_.header.seq = 0;

  emergency_stop_msg_.data = false;

  remote_cmd_time_ = ros::Time::now();
  watchdog_timer_thread_ = std::thread(&TwistGate::watchdog_timer, this);
  watchdog_timer_thread_.detach();
}

TwistGate::~TwistGate()
{
}

void TwistGate::reset_vehicle_cmd_msg()
{
  twist_gate_msg_.linear_x        = 0;
  twist_gate_msg_.angular_z       = 0;
  twist_gate_msg_.mode            = 0;
  twist_gate_msg_.gear            = 0;
  twist_gate_msg_.blinker         = 0;
  twist_gate_msg_.accel           = 0;
  twist_gate_msg_.brake           = 0;
  twist_gate_msg_.steer           = 0;
  twist_gate_msg_.linear_velocity = -1;
  twist_gate_msg_.steering_angle  = 0;
}

void TwistGate::watchdog_timer()
{
  while(1)
  {
    ros::Time now_time = ros::Time::now();
    bool emergency_flag = false;

    // check command mode
    if(previous_command_mode_ != command_mode_) {
      if(command_mode_ == CommandMode::AUTO) {
        command_mode_topic_.data = "AUTO";
      }
      else if(command_mode_ == CommandMode::REMOTE) {
        command_mode_topic_.data = "REMOTE";
      }
      else{
        command_mode_topic_.data = "UNDEFINED";
      }

      control_command_pub_.publish(command_mode_topic_);
      previous_command_mode_ = command_mode_;
    }

    // if lost Communication
    if(command_mode_ == CommandMode::REMOTE && now_time - remote_cmd_time_ >  timeout_period_) {
      emergency_flag = true;
      ROS_WARN("Lost Communication!");
    }

    // if push emergency stop button
    if(emergency_stop_msg_.data == true)
    {
      emergency_flag = true;
      ROS_WARN("Emergency Mode!");
    }

    // Emergency
    if(emergency_flag) {
      command_mode_ = CommandMode::AUTO;
      emergency_stop_msg_.data = true;
      emergency_stop_pub_.publish(emergency_stop_msg_);
      ROS_WARN("Emergency Stop!");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void TwistGate::remote_cmd_callback(const remote_msgs_t::ConstPtr& input_msg)
{
  command_mode_ = static_cast<CommandMode>(input_msg->control_mode);
  emergency_stop_msg_.data = static_cast<bool>(input_msg->emergency);
  remote_cmd_time_ = ros::Time::now();

  if(command_mode_ == CommandMode::REMOTE)
  {
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.linear_x = input_msg->accel;
    twist_gate_msg_.angular_z = 0;
    twist_gate_msg_.steering_angle = input_msg->steer;
    twist_gate_msg_.accel = input_msg->accel;
    twist_gate_msg_.brake = input_msg->brake;
    twist_gate_msg_.steer = input_msg->steer;
    twist_gate_msg_.gear = input_msg->gear;
    twist_gate_msg_.blinker = input_msg->blinker;
    twist_gate_msg_.mode = input_msg->mode;
    twist_gate_msg_.emergency = input_msg->emergency;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_twist_cmd_callback(const geometry_msgs::TwistStamped::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.linear_x = input_msg->twist.linear.x;
    twist_gate_msg_.angular_z = input_msg->twist.angular.z;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_mode_cmd_callback(const tablet_socket_msgs::mode_cmd::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    //TODO:check this if statement
    if(input_msg->mode == -1 || input_msg->mode == 0){
      reset_vehicle_cmd_msg();
    }
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.mode = input_msg->mode;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_gear_cmd_callback(const tablet_socket_msgs::gear_cmd::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    twist_gate_msg_.gear = input_msg->gear;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_accel_cmd_callback(const autoware_msgs::accel_cmd::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.accel = input_msg->accel;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_steer_cmd_callback(const autoware_msgs::steer_cmd::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.steer = input_msg->steer;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_brake_cmd_callback(const autoware_msgs::brake_cmd::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.brake = input_msg->brake;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}

void TwistGate::auto_cmd_ctrl_cmd_callback(const autoware_msgs::ControlCommandStamped::ConstPtr& input_msg)
{
  if(command_mode_ == CommandMode::AUTO)
  {
    twist_gate_msg_.header.frame_id = input_msg->header.frame_id;
    twist_gate_msg_.header.stamp = input_msg->header.stamp;
    twist_gate_msg_.header.seq++;
    twist_gate_msg_.linear_velocity = input_msg->cmd.linear_velocity;
    twist_gate_msg_.steering_angle = input_msg->cmd.steering_angle;
    vehicle_cmd_pub_.publish(twist_gate_msg_);
  }
}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "twist_gate");
  ros::NodeHandle nh;
  ros::NodeHandle private_nh("~");

  TwistGate twist_gate(nh, private_nh);

  ros::spin();
  return 0;
}
