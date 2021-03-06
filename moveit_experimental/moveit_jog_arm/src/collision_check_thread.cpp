/*******************************************************************************
 *      Title     : collision_check_thread.cpp
 *      Project   : moveit_jog_arm
 *      Created   : 1/11/2019
 *      Author    : Brian O'Neil, Andy Zelenak, Blake Anderson
 *
 * BSD 3-Clause License
 *
 * Copyright (c) 2019, Los Alamos National Security, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

#include <moveit_jog_arm/collision_check_thread.h>

namespace moveit_jog_arm
{
// Constructor for the class that handles collision checking
CollisionCheckThread::CollisionCheckThread(const moveit_jog_arm::JogArmParameters& parameters,
                                           const robot_model_loader::RobotModelLoaderPtr& model_loader_ptr)
  : parameters_(parameters)
{
  // MoveIt Setup
  while (ros::ok() && !model_loader_ptr)
  {
    ROS_WARN_THROTTLE_NAMED(5, LOGNAME, "Waiting for a non-null robot_model_loader pointer");
    ros::Duration(WHILE_LOOP_WAIT).sleep();
  }

  planning_scene_monitor_.reset(new planning_scene_monitor::PlanningSceneMonitor(model_loader_ptr));
  planning_scene_monitor_->startSceneMonitor();
  planning_scene_monitor_->startStateMonitor();

  if (planning_scene_monitor_->getPlanningScene())
  {
    planning_scene_monitor_->startSceneMonitor("/planning_scene");
    planning_scene_monitor_->startWorldGeometryMonitor();
    planning_scene_monitor_->startStateMonitor();
  }
  else
  {
    ROS_ERROR_STREAM_NAMED(LOGNAME, "Error in setting up the PlanningSceneMonitor.");
    exit(EXIT_FAILURE);
  }
}

void CollisionCheckThread::startMainLoop(JogArmShared& shared_variables, std::mutex& mutex)
{
  // Reset loop termination flag
  stop_requested_ = false;

  // Init collision request and current state
  collision_detection::CollisionRequest collision_request;
  collision_request.group_name = parameters_.move_group_name;
  collision_request.distance = true;
  collision_detection::CollisionResult collision_result;
  robot_state::RobotState& current_state = planning_scene_monitor_->getPlanningScene()->getCurrentStateNonConst();

  double velocity_scale_coefficient = -log(0.001) / parameters_.collision_proximity_threshold;

  ros::Rate collision_rate(parameters_.collision_check_rate);

  /////////////////////////////////////////////////
  // Spin while checking collisions
  /////////////////////////////////////////////////
  while (ros::ok() && !stop_requested_)
  {
    mutex.lock();
    sensor_msgs::JointState jts = shared_variables.joints;
    mutex.unlock();

    for (std::size_t i = 0; i < jts.position.size(); ++i)
      current_state.setJointPositions(jts.name[i], &jts.position[i]);

    collision_result.clear();
    current_state.updateCollisionBodyTransforms();
    planning_scene_monitor_->getPlanningScene()->checkCollision(collision_request, collision_result, current_state);

    // Scale robot velocity according to collision proximity and user-defined thresholds.
    // I scaled exponentially (cubic power) so velocity drops off quickly after the threshold.
    // Proximity decreasing --> decelerate
    double velocity_scale = 1;

    // If we are far from a collision, velocity_scale should be 1.
    // If we are very close to a collision, velocity_scale should be ~zero.
    // When collision_proximity_threshold is breached, start decelerating exponentially.
    if (collision_result.distance < parameters_.collision_proximity_threshold)
    {
      // velocity_scale = e ^ k * (collision_distance - threshold)
      // k = - ln(0.001) / collision_proximity_threshold
      // velocity_scale should equal one when collision_distance is at collision_proximity_threshold.
      // velocity_scale should equal 0.001 when collision_distance is at zero.
      velocity_scale =
          exp(velocity_scale_coefficient * (collision_result.distance - parameters_.collision_proximity_threshold));
    }

    mutex.lock();
    shared_variables.collision_velocity_scale = velocity_scale;
    mutex.unlock();

    collision_rate.sleep();
  }
}

void CollisionCheckThread::stopMainLoop()
{
  stop_requested_ = true;
}
}  // namespace moveit_jog_arm
