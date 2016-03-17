

#include <joint_limits_interface/joint_limits.h>
#include <joint_limits_interface/joint_limits_urdf.h>
#include <joint_limits_interface/joint_limits_rosparam.h>

#include <controller_manager/controller_manager.h>
#include <controller_manager_msgs/SwitchController.h>

#include <canopen_motor_node/robot_layer.h>

using namespace canopen;

UnitConverter::UnitConverter(const std::string &expression, get_var_func_type var_func)
: var_func_(var_func)
{
    parser_.SetVarFactory(UnitConverter::createVariable, this);

    parser_.DefineConst("pi", M_PI);
    parser_.DefineConst("nan", std::numeric_limits<double>::quiet_NaN());

    parser_.DefineFun("rad2deg", UnitConverter::rad2deg);
    parser_.DefineFun("deg2rad", UnitConverter::deg2rad);
    parser_.DefineFun("norm", UnitConverter::norm);
    parser_.DefineFun("smooth", UnitConverter::smooth);
    parser_.DefineFun("avg", UnitConverter::avg);

    parser_.SetExpr(expression);
}

void UnitConverter::reset(){
    for(variable_ptr_list::iterator it = var_list_.begin(); it != var_list_.end(); ++it){
        **it = std::numeric_limits<double>::quiet_NaN();
    }
}

double* UnitConverter::createVariable(const char *name, void * userdata){
    UnitConverter * uc = static_cast<UnitConverter*>(userdata);
    double *p = uc->var_func_ ? uc->var_func_(name) : 0;
    if(!p){
        p = new double(std::numeric_limits<double>::quiet_NaN());
        uc->var_list_.push_back(variable_ptr(p));
    }
    return p;
}


bool HandleLayer::select(const MotorBase::OperationMode &m){
    CommandMap::iterator it = commands_.find(m);
    if(it == commands_.end()) return false;
    jh_ = it->second;
    return true;
}

HandleLayer::HandleLayer(const std::string &name, const boost::shared_ptr<MotorBase> & motor, const boost::shared_ptr<ObjectStorage> storage,  XmlRpc::XmlRpcValue & options)
: Layer(name + " Handle"), motor_(motor), variables_(storage), jsh_(name, &pos_, &vel_, &eff_), jph_(jsh_, &cmd_pos_), jvh_(jsh_, &cmd_vel_), jeh_(jsh_, &cmd_eff_), jh_(0), forward_command_(false),
  filter_pos_("double"), filter_vel_("double"), filter_eff_("double"), options_(options)
{
   commands_[MotorBase::No_Mode] = 0;

   std::string p2d("rint(rad2deg(pos)*1000)"), v2d("rint(rad2deg(vel)*1000)"), e2d("rint(eff)");
   std::string p2r("deg2rad(obj6064)/1000"), v2r("deg2rad(obj606C)/1000"), e2r("0");

   if(options.hasMember("pos_unit_factor") || options.hasMember("vel_unit_factor") || options.hasMember("eff_unit_factor")){
       const std::string reason("*_unit_factor parameters are not supported anymore, please migrate to conversion functions.");
       ROS_FATAL_STREAM(reason);
       throw std::invalid_argument(reason);
   }

   if(options.hasMember("pos_to_device")) p2d = (const std::string&) options["pos_to_device"];
   if(options.hasMember("pos_from_device")) p2r = (const std::string&) options["pos_from_device"];

   if(options.hasMember("vel_to_device")) v2d = (const std::string&) options["vel_to_device"];
   if(options.hasMember("vel_from_device")) v2r = (const std::string&) options["vel_from_device"];

   if(options.hasMember("eff_to_device")) e2d = (const std::string&) options["eff_to_device"];
   if(options.hasMember("eff_from_device")) e2r = (const std::string&) options["eff_from_device"];

   conv_target_pos_.reset(new UnitConverter(p2d, boost::bind(assignVariable, "pos", &cmd_pos_, _1)));
   conv_target_vel_.reset(new UnitConverter(v2d, boost::bind(assignVariable, "vel", &cmd_vel_, _1)));
   conv_target_eff_.reset(new UnitConverter(e2d, boost::bind(assignVariable, "eff", &cmd_eff_, _1)));

   conv_pos_.reset(new UnitConverter(p2r, boost::bind(&ObjectVariables::getVariable, &variables_, _1)));
   conv_vel_.reset(new UnitConverter(v2r, boost::bind(&ObjectVariables::getVariable, &variables_, _1)));
   conv_eff_.reset(new UnitConverter(e2r, boost::bind(&ObjectVariables::getVariable, &variables_, _1)));
}

HandleLayer::CanSwitchResult HandleLayer::canSwitch(const MotorBase::OperationMode &m){
    if(!motor_->isModeSupported(m) || commands_.find(m) == commands_.end()){
        return NotSupported;
    }else if(motor_->getMode() == m){
        return NoNeedToSwitch;
    }else if(motor_->getLayerState() == Ready){
        return ReadyToSwitch;
    }else{
        return NotReadyToSwitch;
    }
}

bool HandleLayer::switchMode(const MotorBase::OperationMode &m){
    if(motor_->getMode() != m){
        forward_command_ = false;
        jh_ = 0; // disconnect handle
        if(!motor_->enterModeAndWait(m)){
            ROS_ERROR_STREAM(jsh_.getName() << "could not enter mode " << (int)m);
            LayerStatus s;
            motor_->halt(s);
            return false;
        }
    }
    return select(m);
}

bool HandleLayer::forwardForMode(const MotorBase::OperationMode &m){
    if(motor_->getMode() == m){
        forward_command_ = true;
        return true;
    }
    return false;
}


bool HandleLayer::registerHandle(hardware_interface::PositionJointInterface &iface){
    std::vector<MotorBase::OperationMode> modes;
    modes.push_back(MotorBase::Profiled_Position);
    modes.push_back(MotorBase::Interpolated_Position);
    modes.push_back(MotorBase::Cyclic_Synchronous_Position);
    return addHandle(iface, &jph_, modes);
}

bool HandleLayer::registerHandle(hardware_interface::VelocityJointInterface &iface){
    std::vector<MotorBase::OperationMode> modes;
    modes.push_back(MotorBase::Velocity);
    modes.push_back(MotorBase::Profiled_Velocity);
    modes.push_back(MotorBase::Cyclic_Synchronous_Velocity);
    return addHandle(iface, &jvh_, modes);
}

bool HandleLayer::registerHandle(hardware_interface::EffortJointInterface &iface){
    std::vector<MotorBase::OperationMode> modes;
    modes.push_back(MotorBase::Profiled_Torque);
    modes.push_back(MotorBase::Cyclic_Synchronous_Torque);
    return addHandle(iface, &jeh_, modes);
}

void HandleLayer::handleRead(LayerStatus &status, const LayerState &current_state) {
    if(current_state > Shutdown){
        variables_.sync();
        filter_pos_.update(conv_pos_->evaluate(), pos_);
        filter_vel_.update(conv_vel_->evaluate(), vel_);
        filter_eff_.update(conv_eff_->evaluate(), eff_);
    }
}
void HandleLayer::handleWrite(LayerStatus &status, const LayerState &current_state) {
    if(current_state == Ready){
        LimitedJointHandle* jh = 0;
        if(forward_command_) jh = jh_;
        
        if(jh == &jph_){
            motor_->setTarget(conv_target_pos_->evaluate());
            cmd_vel_ = vel_;
            cmd_eff_ = eff_;
        }else if(jh == &jvh_){
            motor_->setTarget(conv_target_vel_->evaluate());
            cmd_pos_ = pos_;
            cmd_eff_ = eff_;
        }else if(jh == &jeh_){
            motor_->setTarget(conv_target_eff_->evaluate());
            cmd_pos_ = pos_;
            cmd_vel_ = vel_;
        }else{
            cmd_pos_ = pos_;
            cmd_vel_ = vel_;
            cmd_eff_ = eff_;
            if(jh) status.warn("unsupported mode active");
        }
    }
}

bool prepareFilter(const std::string& joint_name, const std::string& filter_name,  filters::FilterChain<double> &filter, XmlRpc::XmlRpcValue & options, canopen::LayerStatus &status){
    filter.clear();
    if(options.hasMember(filter_name)){
        if(!filter.configure(options[filter_name],joint_name + "/" + filter_name)){
            status.error("could not configure " + filter_name+ " for " + joint_name);
            return false;
        }
    }

    return true;
}

bool HandleLayer::prepareFilters(canopen::LayerStatus &status){
    return prepareFilter(jsh_.getName(), "position_filters", filter_pos_, options_, status) &&
       prepareFilter(jsh_.getName(), "velocity_filters", filter_vel_, options_, status) &&
       prepareFilter(jsh_.getName(), "effort_filters", filter_eff_, options_, status);
}

void HandleLayer::handleInit(LayerStatus &status){
    // TODO: implement proper init
    conv_pos_->reset();
    conv_vel_->reset();
    conv_eff_->reset();
    conv_target_pos_->reset();
    conv_target_vel_->reset();
    conv_target_eff_->reset();


    limits_.limits_flags = 0; // reset
    // TODO: fill hardware limits
    ros::NodeHandle nh;
    LimitedJointHandle::Limits yaml_limits(jsh_.getName(), nh, true);
    limits_.merge(yaml_limits);
    overlay_limits_ = limits_;

    if(prepareFilters(status))
    {
        handleRead(status, Layer::Ready);
    }
}

void HandleLayer::setOverlayLimits(const LimitedJointHandle::Limits & limits){
    overlay_limits_ = LimitedJointHandle::Limits(limits_, limits);
}

void HandleLayer::enforceLimits(const ros::Duration &period, bool recover) {
    LimitedJointHandle * jh = jh_;

    if(jh){
        if(recover) jh->recover();
        jh->enforceLimits(period, overlay_limits_);
    }
}

void RobotLayer::stopControllers(const std::vector<std::string> controllers){
    controller_manager_msgs::SwitchController srv;
    srv.request.stop_controllers = controllers;
    srv.request.strictness = srv.request.BEST_EFFORT;
    boost::thread call(boost::bind(ros::service::call<controller_manager_msgs::SwitchController>, "controller_manager/switch_controller", srv));
    call.detach();
}

void RobotLayer::add(const std::string &name, boost::shared_ptr<HandleLayer> handle){
    LayerGroupNoDiag::add(handle);
    handles_.insert(std::make_pair(name, handle));
}

RobotLayer::RobotLayer(ros::NodeHandle nh, urdf::Model &urdf) : LayerGroupNoDiag<HandleLayer>("RobotLayer"), nh_(nh), urdf_(urdf), first_init_(true)
{
    registerInterface(&state_interface_);
    registerInterface(&pos_interface_);
    registerInterface(&vel_interface_);
    registerInterface(&eff_interface_);
}

void RobotLayer::handleInit(LayerStatus &status){
    if(first_init_){
        for(HandleMap::iterator it = handles_.begin(); it != handles_.end(); ++it){
            it->second->registerHandle(state_interface_);
            it->second->registerHandle(pos_interface_);
            it->second->registerHandle(vel_interface_);
            it->second->registerHandle(eff_interface_);

        }
        first_init_ = false;
    }
    LayerGroupNoDiag::handleInit(status);
}

void RobotLayer::enforceLimits(const ros::Duration &period, bool recover){
    for(HandleMap::iterator it = handles_.begin(); it != handles_.end(); ++it) {
        it->second->enforceLimits(period, recover);
    }
}

bool RobotLayer::prepareSwitch(const std::list<hardware_interface::ControllerInfo> &start_list, const std::list<hardware_interface::ControllerInfo> &stop_list) {
    // stop handles
    for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it = stop_list.begin(); controller_it != stop_list.end(); ++controller_it){

        if(switch_map_.find(controller_it->name) == switch_map_.end()){
            ROS_ERROR_STREAM(controller_it->name << " was not started before");
            return false;
        }
    }

    // start handles
    for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it = start_list.begin(); controller_it != start_list.end(); ++controller_it){
        SwitchContainer to_switch;
        ros::NodeHandle nh(nh_,controller_it->name);
        int mode;
        if(nh.getParam("required_drive_mode", mode)){
            for (std::set<std::string>::const_iterator res_it = controller_it->resources.begin(); res_it != controller_it->resources.end(); ++res_it){
                std::string joint_name = *res_it;
                boost::unordered_map< std::string, boost::shared_ptr<HandleLayer> >::const_iterator h_it = handles_.find(joint_name);

                if(h_it == handles_.end()){
                    ROS_ERROR_STREAM(joint_name << " not found");
                    return false;
                }
                HandleLayer::CanSwitchResult res = h_it->second->canSwitch((MotorBase::OperationMode)mode);

                switch(res){
                    case HandleLayer::NotSupported:
                        ROS_ERROR_STREAM("Mode " << mode << " is not available for " << joint_name);
                        return false;
                    case HandleLayer::NotReadyToSwitch:
                        ROS_ERROR_STREAM(joint_name << " is not ready to switch mode");
                        return false;
                    case HandleLayer::ReadyToSwitch:
                    case HandleLayer::NoNeedToSwitch:
                        {
                            LimitedJointHandle::Limits controller_limits(urdf_.getJoint(joint_name)); // URDF
                            controller_limits.apply(joint_name, nh_, true);      // global YAML
                            controller_limits.apply(joint_name, nh, true);       // local YAML

                            SwitchData data = { h_it->second, MotorBase::OperationMode(mode), LimitedJointHandle::Limits(h_it->second->getLimits(), controller_limits)};
                            to_switch.push_back(data);
                        }
                }
            }
        }else if (controller_it->resources.size()){
            ROS_WARN_STREAM("controller " << controller_it->name << " claims resources, but does not set required_drive_mode param");
        }
        switch_map_.insert(std::make_pair(controller_it->name, to_switch));
    }

    // perform mode switches
    boost::unordered_set<boost::shared_ptr<HandleLayer> > to_stop;
    std::vector<std::string> failed_controllers;
    for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it = stop_list.begin(); controller_it != stop_list.end(); ++controller_it){
        SwitchContainer &to_switch = switch_map_.at(controller_it->name);
        for(RobotLayer::SwitchContainer::iterator it = to_switch.begin(); it != to_switch.end(); ++it){
            to_stop.insert(it->handle);
        }
    }
    for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it = start_list.begin(); controller_it != start_list.end(); ++controller_it){
        SwitchContainer &to_switch = switch_map_.at(controller_it->name);
        bool okay = true;
        for(RobotLayer::SwitchContainer::iterator it = to_switch.begin(); it != to_switch.end(); ++it){
            it->handle->switchMode(MotorBase::No_Mode); // stop all
        }
        for(RobotLayer::SwitchContainer::iterator it = to_switch.begin(); it != to_switch.end(); ++it){
            if(!it->handle->switchMode(it->mode)){
                failed_controllers.push_back(controller_it->name);
                ROS_ERROR_STREAM("Could not switch one joint for " << controller_it->name << ", will stop all related joints and the controller.");
                for(RobotLayer::SwitchContainer::iterator stop_it = to_switch.begin(); stop_it != to_switch.end(); ++stop_it){
                    to_stop.insert(stop_it->handle);
                }
                okay = false;
                break;
            }
            to_stop.erase(it->handle);
        }
    }
    for(boost::unordered_set<boost::shared_ptr<HandleLayer> >::iterator it = to_stop.begin(); it != to_stop.end(); ++it){
        (*it)->switchMode(MotorBase::No_Mode);
        (*it)->setOverlayLimits(LimitedJointHandle::Limits()); // reset limits
    }
    if(!failed_controllers.empty()){
        stopControllers(failed_controllers);
        // will not return false here since this would prevent other controllers to be started and therefore lead to an inconsistent state
    }

    return true;
}

void RobotLayer::doSwitch(const std::list<hardware_interface::ControllerInfo> &start_list, const std::list<hardware_interface::ControllerInfo> &stop_list) {
    std::vector<std::string> failed_controllers;
    for (std::list<hardware_interface::ControllerInfo>::const_iterator controller_it = start_list.begin(); controller_it != start_list.end(); ++controller_it){
        try{
            SwitchContainer &to_switch = switch_map_.at(controller_it->name);
            for(RobotLayer::SwitchContainer::iterator it = to_switch.begin(); it != to_switch.end(); ++it){
                if(!it->handle->forwardForMode(it->mode)){
                    failed_controllers.push_back(controller_it->name);
                    ROS_ERROR_STREAM("Could not switch one joint for " << controller_it->name << ", will stop all related joints and the controller.");
                    for(RobotLayer::SwitchContainer::iterator stop_it = to_switch.begin(); stop_it != to_switch.end(); ++stop_it){
                        it->handle->switchMode(MotorBase::No_Mode);
                    }
                    break;
                }
                it->handle->setOverlayLimits(it->limits);
            }

        }catch(const std::out_of_range&){
            ROS_ERROR_STREAM("Conttroller " << controller_it->name << "not found, will stop it");
            failed_controllers.push_back(controller_it->name);
        }
    }
    if(!failed_controllers.empty()){
        stopControllers(failed_controllers);
    }
}


void ControllerManagerLayer::handleRead(canopen::LayerStatus &status, const LayerState &current_state) {
    if(current_state > Shutdown){
        if(!cm_) status.error("controller_manager is not intialized");
    }
}

void ControllerManagerLayer::handleWrite(canopen::LayerStatus &status, const LayerState &current_state) {
    if(current_state > Shutdown){
        if(!cm_){
            status.error("controller_manager is not intialized");
        }else{
            canopen::time_point abs_now = canopen::get_abs_time();
            ros::Time now = ros::Time::now();

            ros::Duration period = fixed_period_;

            if(period.isZero()) {
                period.fromSec(boost::chrono::duration<double>(abs_now -last_time_).count());
            }

            last_time_ = abs_now;

            bool recover = recover_.exchange(false);
            cm_->update(now, period, recover);
            robot_->enforceLimits(period, recover);
        }
    }
}

void ControllerManagerLayer::handleInit(canopen::LayerStatus &status) {
    if(cm_){
        status.warn("controller_manager is already intialized");
    }else{
        recover_ = true;
        last_time_ = canopen::get_abs_time();
        cm_.reset(new controller_manager::ControllerManager(robot_.get(), nh_));
    }
}

void ControllerManagerLayer::handleRecover(canopen::LayerStatus &status) {
    if(!cm_) status.error("controller_manager is not intialized");
    else recover_ = true;
}

void ControllerManagerLayer::handleShutdown(canopen::LayerStatus &status) {
    cm_.reset();
}
