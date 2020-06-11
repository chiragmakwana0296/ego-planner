
#include <plan_manage/rebo_replan_fsm.h>

namespace rebound_planner {

void ReboReplanFSM::init(ros::NodeHandle& nh) {
  current_wp_  = 0;
  exec_state_  = FSM_EXEC_STATE::INIT;
  have_target_ = false;
  have_odom_   = false;

  /*  fsm param  */
  nh.param("fsm/flight_type", target_type_, -1); 
  nh.param("fsm/thresh_replan", replan_thresh_, -1.0);
  nh.param("fsm/thresh_no_replan", no_replan_thresh_, -1.0);
  nh.param("fsm/planning_horizen", planning_horizen_, -1.0);
  nh.param("fsm/planning_horizen_time", planning_horizen_time_, -1.0);

  nh.param("fsm/waypoint_num", waypoint_num_, -1);
  for (int i = 0; i < waypoint_num_; i++) {
    nh.param("fsm/waypoint" + to_string(i) + "_x", waypoints_[i][0], -1.0);
    nh.param("fsm/waypoint" + to_string(i) + "_y", waypoints_[i][1], -1.0);
    nh.param("fsm/waypoint" + to_string(i) + "_z", waypoints_[i][2], -1.0);
  }

  /* initialize main modules */
  visualization_.reset(new PlanningVisualization(nh));
  planner_manager_.reset(new ReboundPlannerManager);
  planner_manager_->initPlanModules(nh, visualization_);

  /* callback */
  exec_timer_   = nh.createTimer(ros::Duration(0.01), &ReboReplanFSM::execFSMCallback, this);
  safety_timer_ = nh.createTimer(ros::Duration(0.05), &ReboReplanFSM::checkCollisionCallback, this);

  waypoint_sub_ =
      nh.subscribe("/waypoint_generator/waypoints", 1, &ReboReplanFSM::waypointCallback, this);
  odom_sub_ = nh.subscribe("/odom_world", 1, &ReboReplanFSM::odometryCallback, this);

  replan_pub_  = nh.advertise<std_msgs::Empty>("/planning/replan", 10);
  new_pub_     = nh.advertise<std_msgs::Empty>("/planning/new", 10);
  bspline_pub_ = nh.advertise<rebound_planner::Bspline>("/planning/bspline", 10);
  data_disp_pub_ = nh.advertise<rebound_planner::DataDisp>("/planning/data_display", 100);
}

void ReboReplanFSM::waypointCallback(const nav_msgs::PathConstPtr& msg) {
  if (msg->poses[0].pose.position.z < -0.1) return;

  cout << "Triggered!" << endl;
  trigger_ = true;

  if (target_type_ == TARGET_TYPE::MANUAL_TARGET) {
    end_pt_ << msg->poses[0].pose.position.x, msg->poses[0].pose.position.y, 1.0;

  } else if (target_type_ == TARGET_TYPE::PRESET_TARGET) {
    end_pt_(0)  = waypoints_[current_wp_][0];
    end_pt_(1)  = waypoints_[current_wp_][1];
    end_pt_(2)  = waypoints_[current_wp_][2];
    current_wp_ = (current_wp_ + 1) % waypoint_num_;
  }

  init_pt_ = odom_pos_;

  bool success = planner_manager_->planGlobalTraj( odom_pos_, odom_vel_, Eigen::Vector3d::Zero(), end_pt_, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero() );
  if ( success )
  {

    /*** display ***/
    vector<Eigen::Vector3d> gloabl_traj;
    for ( double t=0; t<planner_manager_->global_data_.global_duration_; t+=1 )
      gloabl_traj.push_back( planner_manager_->global_data_.global_traj_.evaluate(t) );

    visualization_->displayInitList(gloabl_traj, 0.1, 1234);

    visualization_->drawGoal(end_pt_, 0.3, Eigen::Vector4d(1, 0, 0, 1.0));
    end_vel_.setZero();
    have_target_ = true;
    have_new_target_ = true;

    /*** FSM ***/
    if (exec_state_ == WAIT_TARGET)
      changeFSMExecState(GEN_NEW_TRAJ, "TRIG");
    else if (exec_state_ == EXEC_TRAJ)
      changeFSMExecState(REPLAN_TRAJ, "TRIG");
  }
  else
  {
    ROS_ERROR( "Unable to generate global trajectory!" );
  }
  
}

void ReboReplanFSM::odometryCallback(const nav_msgs::OdometryConstPtr& msg) {
  odom_pos_(0) = msg->pose.pose.position.x;
  odom_pos_(1) = msg->pose.pose.position.y;
  odom_pos_(2) = msg->pose.pose.position.z;

  odom_vel_(0) = msg->twist.twist.linear.x;
  odom_vel_(1) = msg->twist.twist.linear.y;
  odom_vel_(2) = msg->twist.twist.linear.z;

  //odom_acc_ = estimateAcc( msg );

  odom_orient_.w() = msg->pose.pose.orientation.w;
  odom_orient_.x() = msg->pose.pose.orientation.x;
  odom_orient_.y() = msg->pose.pose.orientation.y;
  odom_orient_.z() = msg->pose.pose.orientation.z;

  have_odom_ = true;
}

void ReboReplanFSM::changeFSMExecState(FSM_EXEC_STATE new_state, string pos_call) {
  
  if ( new_state == exec_state_ )
    continously_called_times_ ++;
  else
    continously_called_times_ = 1;

  static string state_str[7] = { "INIT", "WAIT_TARGET", "GEN_NEW_TRAJ", "REPLAN_TRAJ", "EXEC_TRAJ", "EMERGENCY_STOP" };
  int    pre_s        = int(exec_state_);
  exec_state_         = new_state;
  cout << "[" + pos_call + "]: from " + state_str[pre_s] + " to " + state_str[int(new_state)] << endl;
  
}

std::pair<int, ReboReplanFSM::FSM_EXEC_STATE> ReboReplanFSM::timesOfConsecutiveStateCalls()
{
  return std::pair<int, FSM_EXEC_STATE>(continously_called_times_, exec_state_);
}

void ReboReplanFSM::printFSMExecState() {
  static string state_str[7] = { "INIT", "WAIT_TARGET", "GEN_NEW_TRAJ", "REPLAN_TRAJ", "EXEC_TRAJ", "EMERGENCY_STOP" };

  cout << "[FSM]: state: " + state_str[int(exec_state_)] << endl;
}

void ReboReplanFSM::execFSMCallback(const ros::TimerEvent& e) {

  static int fsm_num = 0;
  fsm_num++;
  if (fsm_num == 100) {
    printFSMExecState();
    if (!have_odom_) cout << "no odom." << endl;
    if (!trigger_) cout << "wait for goal." << endl;
    fsm_num = 0;
  }

  switch (exec_state_) {
    case INIT: {
      if (!have_odom_) {
        return;
      }
      if (!trigger_) {
        return;
      }
      changeFSMExecState(WAIT_TARGET, "FSM");
      break;
    }

    case WAIT_TARGET: {
      if (!have_target_)
        return;
      else {
        changeFSMExecState(GEN_NEW_TRAJ, "FSM");
      }
      break;
    }

    case GEN_NEW_TRAJ: {
      start_pt_  = odom_pos_;
      start_vel_ = odom_vel_;
      start_acc_.setZero();

      // Eigen::Vector3d rot_x = odom_orient_.toRotationMatrix().block(0, 0, 3, 1);
      // start_yaw_(0)         = atan2(rot_x(1), rot_x(0));
      // start_yaw_(1) = start_yaw_(2) = 0.0;

      bool flag_random_poly_init;
      if ( timesOfConsecutiveStateCalls().first == 1 ) 
        flag_random_poly_init = false;
      else
        flag_random_poly_init = true;

      bool success = callReboundReplan(true, flag_random_poly_init);
      if (success) {

        changeFSMExecState(EXEC_TRAJ, "FSM");
        flag_escape_emergency_ = true;
      } else {
        changeFSMExecState(GEN_NEW_TRAJ, "FSM");
      }
      break;
    }

    case EXEC_TRAJ: {
      /* determine if need to replan */
      LocalTrajData* info     = &planner_manager_->local_data_;
      ros::Time      time_now = ros::Time::now();
      double         t_cur    = (time_now - info->start_time_).toSec();
      t_cur                   = min(info->duration_, t_cur);

      Eigen::Vector3d pos = info->position_traj_.evaluateDeBoorT(t_cur);

      /* && (end_pt_ - pos).norm() < 0.5 */
      if (t_cur > info->duration_ - 1e-2) {
        have_target_ = false;
        changeFSMExecState(WAIT_TARGET, "FSM");
        return;

      } else if ((end_pt_ - pos).norm() < no_replan_thresh_) {
        // cout << "near end" << endl;
        return;

      } else if ((info->start_pos_ - pos).norm() < replan_thresh_) {
        // cout << "near start" << endl;
        return;

      } else {
        changeFSMExecState(REPLAN_TRAJ, "FSM");
      }
      break;
    }


    case REPLAN_TRAJ: {

      if (planFromCurrentTraj() )
      {
        changeFSMExecState(EXEC_TRAJ, "FSM");
      }
      else
      {
        changeFSMExecState(REPLAN_TRAJ, "FSM");
      }
      
      break;
    }

    case EMERGENCY_STOP: {

      if ( flag_escape_emergency_ ) // Avoiding repeated calls
      {
        callEmergencyStop( odom_pos_ );
      }
      else
      {
        if ( odom_vel_.norm() < 0.1 )
          changeFSMExecState(GEN_NEW_TRAJ, "FSM");
      }
      
      flag_escape_emergency_ = false;
      break;
    }
  }

  data_disp_.header.stamp = ros::Time::now();
  data_disp_pub_.publish(data_disp_);

}

bool ReboReplanFSM::planFromCurrentTraj()
{
  LocalTrajData* info     = &planner_manager_->local_data_;
  ros::Time      time_now = ros::Time::now();
  double         t_cur    = (time_now - info->start_time_).toSec();

  start_pt_  = info->position_traj_.evaluateDeBoorT(t_cur);
  start_vel_ = info->velocity_traj_.evaluateDeBoorT(t_cur);
  start_acc_ = info->acceleration_traj_.evaluateDeBoorT(t_cur);

  bool success = callReboundReplan(false, false);
  if ( !success) 
  {
    success = callReboundReplan(true, false);
    //changeFSMExecState(EXEC_TRAJ, "FSM");
    if ( !success )
    {
      success = callReboundReplan(true, true);
      if ( !success )
      {
        return false;
      }
    }
  } 

  return true;
}

void ReboReplanFSM::checkCollisionCallback(const ros::TimerEvent& e) {
  LocalTrajData* info = &planner_manager_->local_data_;
  auto map = planner_manager_->sdf_map_;

  if ( exec_state_ == WAIT_TARGET )
    return;

  /* ---------- check trajectory ---------- */
  constexpr double time_step = 0.01;
  double t_cur = ( ros::Time::now() - info->start_time_ ).toSec();
  for ( double t=t_cur; t<info->duration_; t+=time_step )
  {
    if ( map->getInflateOccupancy( info->position_traj_.evaluateDeBoorT(t) ) )
    {
      if ( planFromCurrentTraj() )  // Make a chance
      {
        changeFSMExecState(EXEC_TRAJ, "SAFETY");
        return;
      }
      else
      {
        if ( t-t_cur < 0.8 ) // 0.8s of emergency time
        {
          ROS_ERROR("Got no time to avoid obstacles. emergency stop! time=%f",t-t_cur);
          changeFSMExecState(EMERGENCY_STOP, "SAFETY");
        }
        else
        {
          ROS_WARN("current traj in collision, replan.");
          changeFSMExecState(REPLAN_TRAJ, "SAFETY");
        }
        return;
      }
      break;
    } 
  }
}

bool ReboReplanFSM::callReboundReplan(bool flag_use_poly_init, bool flag_randomPolyTraj) {
  
  getLocalTarget();

  bool plan_success =
      planner_manager_->reboundReplan(start_pt_, start_vel_, start_acc_, local_target_pt_, local_target_vel_, (have_new_target_ || flag_use_poly_init), flag_randomPolyTraj);
  have_new_target_ = false;

  cout << "final_plan_success=" << plan_success << endl;

  if (plan_success) {

    auto info = &planner_manager_->local_data_;

    /* publish traj */
    rebound_planner::Bspline bspline;
    bspline.order      = 3;
    bspline.start_time = info->start_time_;
    bspline.traj_id    = info->traj_id_;

    Eigen::MatrixXd pos_pts = info->position_traj_.getControlPoint();

    for (int i = 0; i < pos_pts.rows(); ++i) {
      geometry_msgs::Point pt;
      pt.x = pos_pts(i, 0);
      pt.y = pos_pts(i, 1);
      pt.z = pos_pts(i, 2);
      bspline.pos_pts.push_back(pt);
    }

    Eigen::VectorXd knots = info->position_traj_.getKnot();
    for (int i = 0; i < knots.rows(); ++i) {
      bspline.knots.push_back(knots(i));
    }

    bspline_pub_.publish(bspline);

    /* visulization */
    visualization_->drawBspline(info->position_traj_, 0.1, Eigen::Vector4d(1.0, 0, 0.0, 1), false, 0.2,
                                Eigen::Vector4d(1, 0, 0, 1));

  }

  return plan_success;
}

bool ReboReplanFSM::callEmergencyStop( Eigen::Vector3d stop_pos ) {
  
  planner_manager_->EmergencyStop( stop_pos );

  auto info = &planner_manager_->local_data_;

  /* publish traj */
  rebound_planner::Bspline bspline;
  bspline.order      = 3;
  bspline.start_time = info->start_time_;
  bspline.traj_id    = info->traj_id_;

  Eigen::MatrixXd pos_pts = info->position_traj_.getControlPoint();

  for (int i = 0; i < pos_pts.rows(); ++i) {
    geometry_msgs::Point pt;
    pt.x = pos_pts(i, 0);
    pt.y = pos_pts(i, 1);
    pt.z = pos_pts(i, 2);
    bspline.pos_pts.push_back(pt);
  }

  Eigen::VectorXd knots = info->position_traj_.getKnot();
  for (int i = 0; i < knots.rows(); ++i) {
    bspline.knots.push_back(knots(i));
  }

  bspline_pub_.publish(bspline);

  /* visulization */
  visualization_->drawBspline(info->position_traj_, 0.1, Eigen::Vector4d(1.0, 0, 0.0, 1), false, 0.2,
                              Eigen::Vector4d(1, 0, 0, 1));

  return true;
}

void ReboReplanFSM::getLocalTarget()
{
  if ( ( end_pt_ - start_pt_ ).norm() < planning_horizen_ )
  {
    local_target_pt_ = end_pt_;
  }
  else
  {
    //local_target_pt_ = start_pt_ + (end_pt_ - start_pt_).normalized() * planning_horizen_;

    Eigen::Vector3d M = init_pt_, N = end_pt_ - init_pt_; // line: X = M + N*t
    Eigen::Vector3d X0 = start_pt_; double h = planning_horizen_; // sphere: (X-X0)'*(X-X0)=h
    double a = N.squaredNorm();
    double b = 2*(M-X0).dot(N);
    double c = (M-X0).squaredNorm() - h*h;
    // cout << "M=" << M.transpose() << " N=" << N.transpose() << " X0=" << X0.transpose() << " h=" << h << endl;
    // cout << "a=" << a << " b=" << b << " c=" << c << endl;
    if ( b*b-4*a*c > 0 )
    {
      double t = (-b + sqrt( b*b-4*a*c )) / (2*a);
      local_target_pt_ = M + N*t;
    }
    else
    {
      ROS_WARN("the drone goes too far to the stright line.");
      double t = -b/(2*a);
      local_target_pt_ = M + N*t;
      //local_target_pt_ = start_pt_ + (end_pt_ - start_pt_).normalized() * planning_horizen_;
    }
  }

  if ( ( end_pt_ - local_target_pt_ ).norm() < (planner_manager_->pp_.max_vel_*planner_manager_->pp_.max_vel_)/(2*planner_manager_->pp_.max_acc_) )
  {
    local_target_vel_ = (end_pt_ - init_pt_).normalized() * planner_manager_->pp_.max_vel_ * (( end_pt_ - local_target_pt_ ).norm() / ((planner_manager_->pp_.max_vel_*planner_manager_->pp_.max_vel_)/(2*planner_manager_->pp_.max_acc_)));
  }
  else
  {
    local_target_vel_ = (end_pt_ - init_pt_).normalized() * planner_manager_->pp_.max_vel_; 
  }
  
}

}  // namespace rebound_planner