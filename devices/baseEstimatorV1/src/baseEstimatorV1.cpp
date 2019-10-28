/*
 * Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms of the
 * GNU Lesser General Public License v2.1 or any later version.
 */

#include <baseEstimatorV1.h>


bool yarp::dev::baseEstimatorV1::getJointNamesList(const yarp::os::Searchable& config, std::vector< std::string >& joint_list)
{
    yarp::os::Property property;
    property.fromString(config.toString().c_str());

    yarp::os::Bottle *property_joints_list = property.find("joints_list").asList();
    if (property_joints_list == 0)
    {
        yError() << "floatingBaseEstimatorV1: " <<  "Could not find \"joints_list\" parameter in configuration file.";
        return false;
    }

    joint_list.resize(property_joints_list->size());
    for (int joint_idx = 0; joint_idx < property_joints_list->size(); joint_idx++)
    {
        joint_list[joint_idx] = property_joints_list->get(joint_idx).asString().c_str();
    }
    return true;
}

void yarp::dev::baseEstimatorV1::resizeBuffers()
{
    m_joint_positions.resize(m_model);
    m_joint_velocities.resize(m_joint_positions.size());
    m_joint_velocities.zero();

    m_initial_attitude_estimate_as_quaternion.resize(4);
    m_initial_attitude_estimate_as_quaternion.zero();
    m_initial_attitude_estimate_as_quaternion(0) = 1.0;

    m_world_pose_base_in_R6.resize(6);
    m_world_velocity_base.resize(6);
    m_world_velocity_base_from_imu.resize(6);
    m_world_H_base.resize(4, 4);
    m_left_foot_cartesian_wrench.resize(6);
    m_right_foot_cartesian_wrench.resize(6);

    m_imu_attitude_estimate_as_rpy.zero();

    m_world_l_foot_pose_in_R6.resize(6);
    m_world_r_foot_pose_in_R6.resize(6);
    m_world_H_lf.resize(4, 4);
    m_world_H_rf.resize(4, 4);

    m_feet_imu_confidence.resize(3);

    // wbd contact wrenches, ft sensors and imu measurement buffers are resized in the respective attach methods.
}

yarp::dev::baseEstimatorV1::baseEstimatorV1(double period, yarp::os::ShouldUseSystemClock useSystemClock): PeriodicThread(period, useSystemClock)
{
}

yarp::dev::baseEstimatorV1::baseEstimatorV1(): PeriodicThread(0.01, yarp::os::ShouldUseSystemClock::No)
{
}


bool yarp::dev::baseEstimatorV1::open(yarp::os::Searchable& config)
{
    yarp::os::LockGuard guard(m_device_mutex);
    if (!configureWholeBodyDynamics(config))
    {
        yError() << "floatingBaseEstimatorV1: " <<  "Could not connect to wholebodydynamics";
        return false;
    }

    if (!loadEstimatorParametersFromConfig(config))
    {
        yError() << "floatingBaseEstimatorV1: " <<  "Failed to load settings from configuration file";
        return false;
    }

    if (!openComms())
    {
        return false;
    }

    if (m_dump_data)
    {
        m_logger = std::make_unique<WalkingLogger>();
        yarp::os::Bottle& loggerOptions = config.findGroup("LOGGER");
        if (!m_logger->configure(loggerOptions, "base-estimator"))
        {
            yError() << "[configure] Unable to configure the logger";
            return false;
        }
    }

    m_state = FilterFSM::IDLE;

    return true;
}

bool yarp::dev::baseEstimatorV1::loadLeggedOdometry()
{
    if (!m_model.setDefaultBaseLink(m_model.getFrameIndex(m_base_link_name)))
    {
        yWarning() << "floatingBaseEstimatorV1: " <<  "could not set default base link to " << m_base_link_name <<
                    ". using link " << m_model.getDefaultBaseLink() << " instead.";
    }

    m_legged_odometry = std::make_unique<iDynTree::SimpleLeggedOdometry>();
    if (!m_legged_odometry->setModel(m_model))
    {
        yError() << "floatingBaseEstimatorV1: " << "failed to set model for legged odometry";
        return false;
    }

    int axes{0};
    m_remapped_control_board_interfaces.encs->getAxes(&axes);
    if (axes != (int)m_legged_odometry->model().getNrOfDOFs())
    {
        yError() << "floatingBaseEstimatorV1: " <<  "estimator model and remapped control board interface has inconsistent number of joints";
        return false;
    }

    return true;
}

bool yarp::dev::baseEstimatorV1::loadBipedFootContactClassifier()
{
    m_biped_foot_contact_classifier = std::make_unique<iDynTree::BipedFootContactClassifier>(m_left_foot_contact_schmitt_params, m_right_foot_contact_schmitt_params);
    m_biped_foot_contact_classifier->setContactSwitchingPattern(iDynTree::ALTERNATE_CONTACT);

    return true;
}

bool yarp::dev::baseEstimatorV1::loadIMUAttitudeMahonyEstimator()
{
    m_imu_attitude_observer = std::make_unique<iDynTree::AttitudeMahonyFilter>();
    m_imu_attitude_observer->setGainkp(m_imu_attitude_observer_params.kp);
    m_imu_attitude_observer->setGainki(m_imu_attitude_observer_params.ki);
    m_imu_attitude_observer->setTimeStepInSeconds(m_imu_attitude_observer_params.time_step_in_seconds);
    m_imu_attitude_observer->useMagnetoMeterMeasurements(m_imu_attitude_observer_params.use_magnetometer_measurements);
    return true;
}

bool yarp::dev::baseEstimatorV1::loadIMUAttitudeQEKF()
{
    m_imu_attitude_qekf = std::make_unique<iDynTree::AttitudeQuaternionEKF>();
    m_imu_attitude_qekf->setParameters(m_imu_attitude_qekf_params);
    return true;
}


bool yarp::dev::baseEstimatorV1::initializeLeggedOdometry()
{
    bool ok = m_legged_odometry->updateKinematics(m_joint_positions);
    ok = ok && m_legged_odometry->init(m_initial_fixed_frame, m_initial_reference_frame_for_world, m_initial_reference_frame_H_world);
    yInfo() << "Base link set to: " <<  m_legged_odometry->model().getLinkName(m_legged_odometry->model().getDefaultBaseLink());
    yInfo() << "Initial world to base transform \n" << m_legged_odometry->getWorldLinkTransform(m_legged_odometry->model().getDefaultBaseLink()).toString();

    return ok;
}

bool yarp::dev::baseEstimatorV1::initializeBipedFootContactClassifier()
{
    if (m_initial_primary_foot == "left")
    {
        m_biped_foot_contact_classifier->setPrimaryFoot(iDynTree::BipedFootContactClassifier::LEFT_FOOT);
    }
    else if (m_initial_primary_foot == "right")
    {
        m_biped_foot_contact_classifier->setPrimaryFoot(iDynTree::BipedFootContactClassifier::RIGHT_FOOT);
    }
    else
    {
        yError() << "floatingBaseEstimatorV1: " << "initial primary foot not set.";
    }
    return true;
}

bool yarp::dev::baseEstimatorV1::initializeIMUAttitudeEstimator()
{
    iDynTree::VectorDynSize state;
    state.resize((int)m_imu_attitude_observer->getInternalStateSize());
    state.zero();

    state(0) = m_initial_attitude_estimate_as_quaternion(0);
    state(1) = m_initial_attitude_estimate_as_quaternion(1);
    state(2) = m_initial_attitude_estimate_as_quaternion(2);
    state(3) = m_initial_attitude_estimate_as_quaternion(3);

    iDynTree::Span<double> stateBuf(state.data(), state.size());
    m_imu_attitude_observer->setInternalState(stateBuf);
    return true;
}

bool yarp::dev::baseEstimatorV1::initializeIMUAttitudeQEKF()
{
    if (!m_imu_attitude_qekf->initializeFilter())
    {
        yInfo() <<  "floatingBaseEstimatorV1: " << "Could not initialize Qekf";
        return false;
    }

    iDynTree::Vector10 state;
    state.zero();

    state(0) = m_initial_attitude_estimate_as_quaternion(0);
    state(1) = m_initial_attitude_estimate_as_quaternion(1);
    state(2) = m_initial_attitude_estimate_as_quaternion(2);
    state(3) = m_initial_attitude_estimate_as_quaternion(3);

    iDynTree::Span<double> state_span(state.data(), state.size());
    m_imu_attitude_qekf->setInternalState(state_span);
    // Initial state covariance set during loadAttitudeQEKF

    return true;
}


void yarp::dev::baseEstimatorV1::getFeetCartesianWrenches()
{
    // get these wrenches from whole body dynamics to avoid errors due to calibration offsets
    m_left_foot_contact_normal_force = m_left_foot_cartesian_wrench(2);
    m_right_foot_contact_normal_force = m_right_foot_cartesian_wrench(2);
}


bool yarp::dev::baseEstimatorV1::updateLeggedOdometry()
{
    m_no_foot_in_contact = false;
    m_biped_foot_contact_classifier->updateFootContactState(yarp::os::Time::now(), m_left_foot_contact_normal_force, m_right_foot_contact_normal_force);
    switch (m_biped_foot_contact_classifier->getPrimaryFoot())
    {
        case iDynTree::BipedFootContactClassifier::contactFoot::LEFT_FOOT :
        {
            m_current_fixed_frame = "l_sole";
            break;
        }
        case iDynTree::BipedFootContactClassifier::contactFoot::RIGHT_FOOT :
        {
            m_current_fixed_frame = "r_sole";
            break;
        }
        case iDynTree::BipedFootContactClassifier::contactFoot::UNKNOWN_FOOT :
        {
            m_current_fixed_frame = "unknown";
            m_no_foot_in_contact = true;
            if (m_previous_fixed_frame != "unknown")
            {
                yError() << "floatingBaseEstimatorV1: " << "no foot in contact, failed to set fixed link";
            }
            return false;
        }
    }

    if (m_previous_fixed_frame != m_current_fixed_frame)
    {
        m_legged_odometry_update_went_well = m_legged_odometry->updateKinematics(m_joint_positions);
        iDynTree::Transform w_H_fixedFrame = m_legged_odometry->getWorldFrameTransform(m_legged_odometry->model().getFrameIndex(m_current_fixed_frame));
        iDynTree::Position w_H_fixedFrame_position = w_H_fixedFrame.getPosition();

        // note flat terrain assumption
        w_H_fixedFrame_position(2) = 0;
        w_H_fixedFrame.setPosition(w_H_fixedFrame_position);

        if (m_legged_odometry->changeFixedFrame(m_current_fixed_frame, w_H_fixedFrame))
        {
            yInfo() << "floatingBaseEstimatorV1: " << "legged odometry changed fixed frame to " << m_current_fixed_frame;
        }
        else
        {
            m_legged_odometry_update_went_well = false;
        }
    }

    m_legged_odometry_update_went_well = m_legged_odometry->updateKinematics(m_joint_positions);

    iDynTree::Transform w_H_b = m_legged_odometry->getWorldLinkTransform(m_legged_odometry->model().getDefaultBaseLink());
    //yInfo() <<  "World Pos: " << w_H_b.getPosition().toString();
    yarp::eigen::toEigen(m_world_pose_base_in_R6).block<3,1>(0,0) =  iDynTree::toEigen(w_H_b.getPosition());
    yarp::eigen::toEigen(m_world_pose_base_in_R6).block<3,1>(3,0) =  iDynTree::toEigen(w_H_b.getRotation().asRPY());
    iDynTree::toYarp(w_H_b.asHomogeneousTransform(), m_world_H_base);

    auto l_foot_idx{m_legged_odometry->model().getLinkIndex("l_foot")};
    auto r_foot_idx{m_legged_odometry->model().getLinkIndex("r_foot")};
    auto w_H_lf{m_legged_odometry->getWorldLinkTransform(l_foot_idx)};
    auto w_H_rf{m_legged_odometry->getWorldLinkTransform(r_foot_idx)};
    yarp::eigen::toEigen(m_world_l_foot_pose_in_R6).block<3,1>(0,0) =  iDynTree::toEigen(w_H_lf.getPosition());
    yarp::eigen::toEigen(m_world_l_foot_pose_in_R6).block<3,1>(3,0) =  iDynTree::toEigen(w_H_lf.getRotation().asRPY());
    yarp::eigen::toEigen(m_world_r_foot_pose_in_R6).block<3,1>(0,0) =  iDynTree::toEigen(w_H_rf.getPosition());
    yarp::eigen::toEigen(m_world_r_foot_pose_in_R6).block<3,1>(3,0) =  iDynTree::toEigen(w_H_rf.getRotation().asRPY());
    iDynTree::toYarp(w_H_lf.asHomogeneousTransform(), m_world_H_lf);
    iDynTree::toYarp(w_H_rf.asHomogeneousTransform(), m_world_H_rf);

    return m_legged_odometry_update_went_well;
}

bool yarp::dev::baseEstimatorV1::updateIMUAttitudeEstimator()
{
    iDynTree::RPY rpy;
    for (size_t imu = 0; imu < m_nr_of_IMUs_detected; imu++)
    {
        if (m_raw_IMU_measurements[imu].sensor_name == m_imu_name)
        {
            m_imu_attitude_observer->updateFilterWithMeasurements(m_raw_IMU_measurements[imu].linear_proper_acceleration,
                                                                  m_raw_IMU_measurements[imu].angular_velocity);
            rpy = m_raw_IMU_measurements[imu].orientation;
            break;
        }
    }

    m_imu_attitude_observer->propagateStates();

    if (!m_use_imu_orientation_direct)
    {
        m_imu_attitude_observer->getOrientationEstimateAsRPY(m_imu_attitude_estimate_as_rpy);
    }
    else
    {
        m_imu_attitude_estimate_as_rpy = rpy;
    }
    return true;
}

bool yarp::dev::baseEstimatorV1::updateIMUAttitudeQEKF()
{
    iDynTree::RPY rpy;
    m_imu_attitude_qekf->propagateStates();
    for (size_t imu = 0; imu < m_nr_of_IMUs_detected; imu++)
    {
        if (m_raw_IMU_measurements[imu].sensor_name == m_imu_name)
        {
            m_imu_attitude_qekf->updateFilterWithMeasurements(m_raw_IMU_measurements[imu].linear_proper_acceleration,
                                                              m_raw_IMU_measurements[imu].angular_velocity);
            rpy = m_raw_IMU_measurements[imu].orientation;
            break;
        }
    }

    if (!m_use_imu_orientation_direct)
    {
        m_imu_attitude_observer->getOrientationEstimateAsRPY(m_imu_attitude_estimate_as_rpy);
    }
    else
    {
        m_imu_attitude_estimate_as_rpy = rpy;
    }
    return true;
}

iDynTree::Transform yarp::dev::baseEstimatorV1::getHeadIMU_H_BaseAtZero()
{
    iDynTree::KinDynComputations temp_kin_comp;
    temp_kin_comp.loadRobotModel(m_model);
    iDynTree::JointPosDoubleArray initial_positions{m_joint_positions}, initial_velocities{m_joint_positions};
    initial_positions.zero();
    initial_velocities.zero();
    iDynTree::Vector3 gravity;
    gravity.zero();
    gravity(2) = -9.81;

    bool broke{false};
    int vecDynIdx{0};
    for (auto& upper_body_joint : m_head_to_base_joints_list)
    {
        std::vector<std::string>::iterator iter = std::find(m_estimation_joint_names.begin(), m_estimation_joint_names.end(), upper_body_joint);
        if (iter != m_estimation_joint_names.end())
        {
            size_t idx = std::distance(m_estimation_joint_names.begin(), iter);
            initial_positions(idx) = m_head_to_base_joints_list_zero_pos(vecDynIdx++);
        }
        else
        {
            broke = true;
            break;
        }
    }

    if (broke)
    {
        yWarning() << "floatingBaseEstimatorV1: " << "home joint positions not mentioned for upper body kinematic chain. Using headIMU_H_baseAtZero as Identity" ;
        return iDynTree::Transform::Identity();
    }

    temp_kin_comp.setRobotState(initial_positions, initial_velocities, gravity);
    return temp_kin_comp.getRelativeTransform(m_imu_name, m_head_imu_link);
}

iDynTree::Transform yarp::dev::baseEstimatorV1::getHeadIMUCorrectionWithUpperBodyKinematics()
{
    // this funciton returns imu_H_imuAssumingBaseToZero
    if (!m_imu_aligned)
    {
        m_imu_H_base_at_zero = getHeadIMU_H_BaseAtZero();
        m_imu_aligned = true;
    }
    setRobotStateWithZeroBaseVelocity();
    iDynTree::Transform imu_H_base =  m_kin_dyn_comp.getRelativeTransform(m_imu_name, m_head_imu_link);
    return imu_H_base*(m_imu_H_base_at_zero.inverse());
}


bool yarp::dev::baseEstimatorV1::alignIMUFrames()
{
    setRobotStateWithZeroBaseVelocity();
    iDynTree::Rotation b_R_imu = m_kin_dyn_comp.getRelativeTransform(m_base_link_name, m_imu_name).getRotation();
    iDynTree::Rotation wIMU_R_IMU_0;

    if (m_use_imu_orientation_direct)
    {
        for (size_t imu = 0; imu < m_nr_of_IMUs_detected; imu++)
        {
            if (m_raw_IMU_measurements[imu].sensor_name == m_imu_name)
            {
                wIMU_R_IMU_0 = iDynTree::Rotation::RPY(m_raw_IMU_measurements[imu].orientation(0),
                                                    m_raw_IMU_measurements[imu].orientation(1),
                                                    m_raw_IMU_measurements[imu].orientation(2));
                break;
            }
        }
    }
    else
    {
        if (m_attitude_estimator_type == "qekf")
        {
            m_imu_attitude_qekf->getOrientationEstimateAsRotationMatrix(wIMU_R_IMU_0);
        }
        else if (m_attitude_estimator_type == "mahony")
        {
            m_imu_attitude_observer->getOrientationEstimateAsRotationMatrix(wIMU_R_IMU_0);
        }
        else
        {
            yError() << "floatingBaseEstimatorV1: " << "Not using any attitude estimator, cannot align IMU frames";
        }
    }
    iDynTree::Rotation w_R_b =  iDynTree::Rotation::RPY(m_world_pose_base_in_R6(3), m_world_pose_base_in_R6(4), m_world_pose_base_in_R6(5));
    m_imu_alignment_matrix = w_R_b*b_R_imu*wIMU_R_IMU_0.inverse();

    // align feet imu matrices
    if (m_use_feet_imu)
    {
        for (auto& foot_imu : m_feet_imu_accelerometers)
        {
            for (size_t imu = 0; imu < m_nr_of_IMUs_detected; imu++)
            {
                if (m_raw_IMU_measurements[imu].sensor_name == foot_imu)
                {
                    auto imu_link_idx{m_model.getFrameLink(m_model.getFrameIndex(foot_imu))};
                    auto imu_link{m_model.getLinkName(imu_link_idx)};
                    iDynTree::Rotation imulink_R_imu = m_kin_dyn_comp.getRelativeTransform(imu_link, foot_imu).getRotation();

                    // dirty check : check if first letter is l or r to see left foot or right foot - sorry for this quick hack!
                    // however this obliges urdf naming conventions
                    if (foot_imu.at(0) == 'l')
                    {
                        auto foot_link{"l_foot"};
                        auto footlink_R_imulink = m_kin_dyn_comp.getRelativeTransform(foot_link, imu_link).getRotation();
                        iDynTree::Rotation f_R_imu = footlink_R_imulink*imulink_R_imu;
                        auto left_foot_wimu_R_imu_0 = iDynTree::Rotation::RPY(m_raw_IMU_measurements[imu].orientation(0),
                                                                    m_raw_IMU_measurements[imu].orientation(1),
                                                                    m_raw_IMU_measurements[imu].orientation(2));

                        iDynTree::Rotation w_R_lf =  iDynTree::Rotation::RPY(m_world_l_foot_pose_in_R6(3),
                                                                       m_world_l_foot_pose_in_R6(4),
                                                                       m_world_l_foot_pose_in_R6(5));
                        m_l_foot_imu_alignment_matrix = w_R_lf*f_R_imu*left_foot_wimu_R_imu_0.inverse();

                    }
                    else if (foot_imu.at(0) == 'r')
                    {
                        auto foot_link{"r_foot"};
                        auto footlink_R_imulink = m_kin_dyn_comp.getRelativeTransform(foot_link, imu_link).getRotation();
                        iDynTree::Rotation f_R_imu = footlink_R_imulink*imulink_R_imu;
                        auto right_foot_wimu_R_imu_0 = iDynTree::Rotation::RPY(m_raw_IMU_measurements[imu].orientation(0),
                                                                    m_raw_IMU_measurements[imu].orientation(1),
                                                                    m_raw_IMU_measurements[imu].orientation(2));

                        iDynTree::Rotation w_R_rf =  iDynTree::Rotation::RPY(m_world_r_foot_pose_in_R6(3),
                                                                       m_world_r_foot_pose_in_R6(4),
                                                                       m_world_r_foot_pose_in_R6(5));
                        m_r_foot_imu_alignment_matrix = w_R_rf*f_R_imu*right_foot_wimu_R_imu_0.inverse();
                    }
                    break;
                }
            }
        }
    }

    return true;
}

iDynTree::Rotation yarp::dev::baseEstimatorV1::getBaseOrientationFromIMU()
{
    iDynTree::Rotation wIMU_R_IMU;
    if (m_use_imu_orientation_direct)
    {
        for (size_t imu = 0; imu < m_nr_of_IMUs_detected; imu++)
        {
            if (m_raw_IMU_measurements[imu].sensor_name == m_imu_name)
            {
                wIMU_R_IMU = iDynTree::Rotation::RPY(m_raw_IMU_measurements[imu].orientation(0),
                                                    m_raw_IMU_measurements[imu].orientation(1),
                                                    m_raw_IMU_measurements[imu].orientation(2));
                break;
            }
        }
    }
    else
    {
        if (m_attitude_estimator_type == "mahony")
        {
            m_imu_attitude_observer->getOrientationEstimateAsRotationMatrix(wIMU_R_IMU);
        }
        else if (m_attitude_estimator_type == "qekf")
        {
            m_imu_attitude_qekf->getOrientationEstimateAsRotationMatrix(wIMU_R_IMU);
        }
    }

    iDynTree::Rotation IMU_R_b = m_kin_dyn_comp.getRelativeTransform(m_imu_name, m_base_link_name).getRotation();
    iDynTree::Rotation wIMU_R_b;
    if (m_is_head_imu)
    {
        iDynTree::Rotation wIMU_R_wIMUBaseAtZero = getHeadIMUCorrectionWithUpperBodyKinematics().getRotation();
        // correct IMU with upperbody kinematics
        wIMU_R_b = wIMU_R_wIMUBaseAtZero*wIMU_R_IMU*IMU_R_b;
    }
    else
    {
        wIMU_R_b = wIMU_R_IMU*IMU_R_b;
    }

    return (m_imu_alignment_matrix * wIMU_R_b);
}

iDynTree::Rotation yarp::dev::baseEstimatorV1::getFootOrientationFromIMU(const char& l_or_r)
{
    iDynTree::Rotation w_R_f;
    setRobotStateWithZeroBaseVelocity();
    for (auto& foot_imu : m_feet_imu_accelerometers)
    {
        bool found{false};
        for (size_t imu = 0; imu < m_nr_of_IMUs_detected; imu++)
        {
            if (m_raw_IMU_measurements[imu].sensor_name.at(0) != l_or_r)
            {
                continue;
            }

            if (m_raw_IMU_measurements[imu].sensor_name == foot_imu)
            {
                    // dirty check : check if first letter is l or r to see left foot or right foot - sorry for this quick hack!
                    // however this obliges urdf naming conventions
                    auto imu_link_idx{m_model.getFrameLink(m_model.getFrameIndex(foot_imu))};
                    auto imu_link{m_model.getLinkName(imu_link_idx)};
                    iDynTree::Rotation imulink_R_imu = m_kin_dyn_comp.getRelativeTransform(imu_link, foot_imu).getRotation();

                    iDynTree::Rotation f_R_imu;
                    auto wIMU_R_IMU = iDynTree::Rotation::RPY(m_raw_IMU_measurements[imu].orientation(0),
                                                            m_raw_IMU_measurements[imu].orientation(1),
                                                            m_raw_IMU_measurements[imu].orientation(2));
                    if (l_or_r == 'l')
                    {
                        auto foot_link{"l_foot"};
                        auto footlink_R_imulink = m_kin_dyn_comp.getRelativeTransform(foot_link, imu_link).getRotation();
                        f_R_imu = footlink_R_imulink*imulink_R_imu;
                        w_R_f = m_l_foot_imu_alignment_matrix*wIMU_R_IMU*f_R_imu.inverse();
                    }
                    else if (l_or_r == 'r')
                    {
                        auto foot_link{"r_foot"};
                        auto footlink_R_imulink = m_kin_dyn_comp.getRelativeTransform(foot_link, imu_link).getRotation();
                        f_R_imu = footlink_R_imulink*imulink_R_imu;
                        w_R_f = m_r_foot_imu_alignment_matrix*wIMU_R_IMU*f_R_imu.inverse();
                    }
                    found = true;
                    break;
                }

                if (found)
                {
                    break;
                }
            }
        }

    return w_R_f;
}

bool yarp::dev::baseEstimatorV1::updateBasePoseWithIMUEstimates()
{
    double updated_roll;
    double updated_pitch;

    iDynTree::Rotation w_R_b_imu = getBaseOrientationFromIMU();
    double weight_imu_on_roll{m_imu_confidence_roll}, weight_imu_on_pitch{m_imu_confidence_roll};
    std::string fixed_link;

    if (m_no_foot_in_contact)
    {
        weight_imu_on_pitch = 1.0;
        weight_imu_on_roll = 1.0;
        fixed_link = "r_foot";
    }
    else
    {
        fixed_link = m_current_fixed_frame;
    }

    // update base rotation
    updated_roll = weight_imu_on_roll*w_R_b_imu.asRPY()(0) + (1 - weight_imu_on_roll) * m_world_pose_base_in_R6(3);
    updated_pitch = weight_imu_on_pitch*w_R_b_imu.asRPY()(1) + (1 - weight_imu_on_pitch)* m_world_pose_base_in_R6(4);

    iDynTree::Rotation w_R_b = iDynTree::Rotation::RPY(updated_roll, updated_pitch, m_world_pose_base_in_R6(5));

    // update base position
    iDynTree::Transform w_H_b;
    iDynTree::toiDynTree(m_world_H_base, w_H_b);
    iDynTree::Transform b_H_fl = m_kin_dyn_comp.getRelativeTransform(m_base_link_name, fixed_link);
    iDynTree::Position w_p_fl = (w_H_b*b_H_fl).getPosition();
    iDynTree::Position b_p_fl = b_H_fl.getPosition();
    iDynTree::Position w_p_b = w_p_fl - (w_R_b*b_p_fl);

    w_H_b.setRotation(w_R_b);
    if (m_no_foot_in_contact)
    {
        w_H_b.setPosition(w_p_b);
    }

    yarp::eigen::toEigen(m_world_pose_base_in_R6).block<3,1>(0,0) =  iDynTree::toEigen(w_H_b.getPosition());
    yarp::eigen::toEigen(m_world_pose_base_in_R6).block<3,1>(3,0) =  iDynTree::toEigen(w_H_b.getRotation().asRPY());
    iDynTree::toYarp(w_H_b.asHomogeneousTransform(), m_world_H_base);

    return true;
}

bool yarp::dev::baseEstimatorV1::updateFeetPoseWithIMUEstimates()
{
    double updated_roll;
    double updated_pitch;

    char foot{'l'};
    iDynTree::Rotation w_R_lf_imu = getFootOrientationFromIMU(foot);
    double weight_imu_on_roll{m_feet_imu_confidence(0)}, weight_imu_on_pitch{m_feet_imu_confidence(1)};

    // update lf rotation
    updated_roll = weight_imu_on_roll*w_R_lf_imu.asRPY()(0) + (1 - weight_imu_on_roll) * m_world_l_foot_pose_in_R6(3);
    updated_pitch = weight_imu_on_pitch*w_R_lf_imu.asRPY()(1) + (1 - weight_imu_on_pitch)* m_world_l_foot_pose_in_R6(4);
    iDynTree::Rotation w_R_lf = iDynTree::Rotation::RPY(updated_roll, updated_pitch, m_world_l_foot_pose_in_R6(5));
    iDynTree::Transform w_H_lf;
    iDynTree::toiDynTree(m_world_H_lf, w_H_lf);
    w_H_lf.setRotation(w_R_lf);

    yarp::eigen::toEigen(m_world_l_foot_pose_in_R6).block<3,1>(3,0) =  iDynTree::toEigen(w_H_lf.getRotation().asRPY());
    iDynTree::toYarp(w_H_lf.asHomogeneousTransform(), m_world_H_lf);

    foot = 'r';
    iDynTree::Rotation w_R_rf_imu = getFootOrientationFromIMU(foot);

    // update rf rotation
    updated_roll = weight_imu_on_roll*w_R_rf_imu.asRPY()(0) + (1 - weight_imu_on_roll) * m_world_r_foot_pose_in_R6(3);
    updated_pitch = weight_imu_on_pitch*w_R_rf_imu.asRPY()(1) + (1 - weight_imu_on_pitch)* m_world_r_foot_pose_in_R6(4);
    iDynTree::Rotation w_R_rf = iDynTree::Rotation::RPY(updated_roll, updated_pitch, m_world_r_foot_pose_in_R6(5));
    iDynTree::Transform w_H_rf;
    iDynTree::toiDynTree(m_world_H_rf, w_H_rf);

    w_H_rf.setRotation(w_R_rf);

    yarp::eigen::toEigen(m_world_r_foot_pose_in_R6).block<3,1>(3,0) =  iDynTree::toEigen(w_H_rf.getRotation().asRPY());
    iDynTree::toYarp(w_H_rf.asHomogeneousTransform(), m_world_H_rf);

    return true;
}

bool yarp::dev::baseEstimatorV1::updateBaseVelocity()
{
    using iDynTree::toiDynTree;
    using iDynTree::toEigen;
    using iDynTree::toYarp;

    if (!m_model.getFrameIndex(m_current_fixed_frame))
    {
        return false;
    }

    if (!setRobotStateWithZeroBaseVelocity())
    {
        return false;
    }

    size_t n_joints = m_joint_velocities.size();
    iDynTree::MatrixDynSize contact_jacobian(6, (n_joints + 6));
    iDynTree::MatrixDynSize contact_jacobian_base(6, 6);
    iDynTree::MatrixDynSize contact_jacobian_shape(6, n_joints);


    if (!m_kin_dyn_comp.getFrameFreeFloatingJacobian(m_current_fixed_frame, contact_jacobian))
    {
        yWarning() << "floatingBaseEstimatorV1: Could not compute the contact jacobian";
        return false;
    }

    toEigen(contact_jacobian_base) = toEigen(contact_jacobian).block(0, 0, 6, 6);
    toEigen(contact_jacobian_shape) = toEigen(contact_jacobian).block(0, 6, 6, n_joints);

    auto contact_jacobian_base_inverse{toEigen(contact_jacobian_base).inverse()};
    iDynTree::Vector6 floating_base_velocity;
    toEigen(floating_base_velocity) = -contact_jacobian_base_inverse * toEigen(contact_jacobian_shape) * toEigen(m_joint_velocities);
    toYarp(floating_base_velocity, m_world_velocity_base);
    return true;
}

bool yarp::dev::baseEstimatorV1::setRobotStateWithZeroBaseVelocity()
{
    iDynTree::Transform w_H_b_estimate;
    toiDynTree(m_world_H_base, w_H_b_estimate);

    iDynTree::Vector3 gravity;
    gravity(2) = -9.8;

    // we assume here the base velocity is zero, since the following computation
    // gives us the floating jacobian which is dependent only on the joint positions
    // and floating base pose due to mixed velocity representation
    iDynTree::Twist base_velocity;
    base_velocity.zero();
    m_kin_dyn_comp.setRobotState(w_H_b_estimate, m_joint_positions, base_velocity, m_joint_velocities, gravity);

    return true;
}

bool yarp::dev::baseEstimatorV1::updateBaseVelocityWithIMU()
{
    using iDynTree::toEigen;
    iDynTree::Vector3 y_acc;

    for (size_t imu = 0; imu < (size_t)m_whole_body_imu_interface.size(); imu++)
    {
        if (m_raw_IMU_measurements[imu].sensor_name == m_imu_name)
        {
            y_acc = m_raw_IMU_measurements[imu].linear_proper_acceleration;

            break;
        }
    }

    // w_R_b*b_R_w*w_R_wimu(imu_ang_vel)
    iDynTree::Rotation wIMU_R_IMU;
    iDynTree::Vector10 attitude_estimator_state;
    iDynTree::Span<double> state_buffer(attitude_estimator_state.data(), attitude_estimator_state.size());
    if (m_attitude_estimator_type == "mahony")
    {
        m_imu_attitude_observer->getOrientationEstimateAsRotationMatrix(wIMU_R_IMU);
        m_imu_attitude_observer->getInternalState(state_buffer);
    }
    else if (m_attitude_estimator_type == "qekf")
    {
        m_imu_attitude_qekf->getOrientationEstimateAsRotationMatrix(wIMU_R_IMU);
        m_imu_attitude_qekf->getInternalState(state_buffer);
    }

    iDynTree::Rotation w_R_IMU = m_imu_alignment_matrix*wIMU_R_IMU;
    iDynTree::Vector3 gravity;
    gravity.zero();
    gravity(2) = -9.8;

    iDynTree::Vector6 base_vel;
    // compute left trivialized base velocity
    auto imu_lin_vel{toEigen(base_vel).block<3, 1>(0, 0)};
    imu_lin_vel = imu_lin_vel + m_device_period_in_s*((toEigen(w_R_IMU)*toEigen(y_acc)) + toEigen(gravity));

    iDynTree::Vector3 imu_ang_vel;
    imu_ang_vel(0) = attitude_estimator_state(4);
    imu_ang_vel(1) = attitude_estimator_state(5);
    imu_ang_vel(2) = attitude_estimator_state(6);
    toEigen(base_vel).block<3, 1>(3, 0) = (toEigen(w_R_IMU)*toEigen(imu_ang_vel));

    iDynTree::toYarp(base_vel, m_world_velocity_base_from_imu);
    return true;
}


void yarp::dev::baseEstimatorV1::publishFloatingBaseState()
{
    yarp::os::Bottle &state_bottle = m_floating_base_state_port.prepare();
    state_bottle.clear();
    for (unsigned int i = 0; i < m_world_pose_base_in_R6.size(); i++)
    {
        state_bottle.addDouble(m_world_pose_base_in_R6[i]);
    }

    for (unsigned int i = 0; i < m_joint_positions.size(); i++)
    {
        state_bottle.addDouble(m_joint_positions(i));
    }

    m_floating_base_state_port.write();
}

void yarp::dev::baseEstimatorV1::publishFloatingBasePoseVelocity()
{
    yarp::os::Bottle &state_bottle = m_floating_base_pose_port.prepare();
    state_bottle.clear();
    for (unsigned int i = 0; i < m_world_pose_base_in_R6.size(); i++)
    {
        state_bottle.addDouble(m_world_pose_base_in_R6[i]);
    }

    for (unsigned int i = 0; i < m_world_velocity_base.size(); i++)
    {
        state_bottle.addDouble(m_world_velocity_base[i]);
    }

    m_floating_base_pose_port.write();
}

void yarp::dev::baseEstimatorV1::publishContactState()
{
    yarp::os::Bottle &state_bottle = m_contact_state_port.prepare();
    state_bottle.clear();
    state_bottle.addDouble(m_left_foot_contact_normal_force);
    state_bottle.addDouble(m_right_foot_contact_normal_force);
    state_bottle.addInt(m_biped_foot_contact_classifier->getLeftFootContactState());
    state_bottle.addInt(m_biped_foot_contact_classifier->getRightFootContactState());
    state_bottle.addString(m_current_fixed_frame);
    m_contact_state_port.write();
}

void yarp::dev::baseEstimatorV1::publishFootPose()
{
    yarp::os::Bottle &lf_bottle = m_left_foot_pose_port.prepare();
    yarp::os::Bottle &rf_bottle = m_right_foot_pose_port.prepare();
    lf_bottle.clear(); rf_bottle.clear();
    for (unsigned int i = 0; i < m_world_l_foot_pose_in_R6.size(); i++)
    {
        lf_bottle.addDouble(m_world_l_foot_pose_in_R6[i]);
        rf_bottle.addDouble(m_world_r_foot_pose_in_R6[i]);
    }
    m_left_foot_pose_port.write();
    m_right_foot_pose_port.write();
}

void yarp::dev::baseEstimatorV1::publishIMUAttitudeEstimatorStates()
{
    iDynTree::VectorDynSize attitude_observer_state;
    if (m_attitude_estimator_type == "mahony")
    {
        attitude_observer_state.resize(m_imu_attitude_observer->getInternalStateSize());
    }
    else if (m_attitude_estimator_type == "qekf")
    {
        attitude_observer_state.resize(m_imu_attitude_qekf->getInternalStateSize());
    }

    iDynTree::Span<double> stateBuffer(attitude_observer_state.data(), attitude_observer_state.size());

    if (m_attitude_estimator_type == "mahony")
    {
        m_imu_attitude_observer->getInternalState(stateBuffer);
    }
    else if (m_attitude_estimator_type == "qekf")
    {
        m_imu_attitude_qekf->getInternalState(stateBuffer);
    }

    yarp::os::Bottle &state_bottle = m_imu_attitude_observer_estimated_state_port.prepare();
    state_bottle.clear();
    state_bottle.addDouble(rad2deg(m_imu_attitude_estimate_as_rpy(0))); // orientation roll
    state_bottle.addDouble(rad2deg(m_imu_attitude_estimate_as_rpy(1))); // orientation pitch
    state_bottle.addDouble(rad2deg(m_imu_attitude_estimate_as_rpy(2))); // orientation yaw
    state_bottle.addDouble(rad2deg(attitude_observer_state(4))); // ang vel about x
    state_bottle.addDouble(rad2deg(attitude_observer_state(5))); // ang vel about y
    state_bottle.addDouble(rad2deg(attitude_observer_state(6))); // ang vel about z
    state_bottle.addDouble(rad2deg(attitude_observer_state(7))); // gyro bias about x
    state_bottle.addDouble(rad2deg(attitude_observer_state(8))); // gyro bias about y
    state_bottle.addDouble(rad2deg(attitude_observer_state(9))); // gyro bias about z

    m_imu_attitude_observer_estimated_state_port.write();
}

void yarp::dev::baseEstimatorV1::publishIMUAttitudeQEKFEstimates()
{
    iDynTree::RPY rpy;
    m_imu_attitude_qekf->getOrientationEstimateAsRPY(rpy);

    yarp::os::Bottle &state_bottle = m_imu_attitude_qekf_estimated_state_port.prepare();
    state_bottle.clear();
    state_bottle.addDouble(rad2deg(rpy(0)));
    state_bottle.addDouble(rad2deg(rpy(1)));
    state_bottle.addDouble(rad2deg(rpy(2)));

    m_imu_attitude_qekf_estimated_state_port.write();
}


void yarp::dev::baseEstimatorV1::publishTransform()
{
    m_transform_interface->setTransform(m_robot+"/"+m_base_link_name, "world", m_world_H_base);
    m_transform_interface->setTransform(m_robot+"/l_foot", "world", m_world_H_lf);
    m_transform_interface->setTransform(m_robot+"/r_foot", "world", m_world_H_rf);
}

bool yarp::dev::baseEstimatorV1::initializeLogger()
{
    m_logger->startRecord({"record","fbe_x", "fbe_y", "fbe_z",
                          "fbe_roll", "fbe_pitch", "fbe_yaw",
                          "fbe_v_x", "fbe_v_y", "fbe_v_z",
                          "fbe_omega_x", "fbe_omega_y", "fbe_omega_z",
                          "fbe_lf_contact", "fbe_rf_contact",
                          "fbe_lf_fz", "fbe_rf_fz",
                          "attEst_roll", "attEst_pitch", "attEst_yaw",
                          "fbe_v_x_imu", "fbe_v_y_imu", "fbe_v_z_imu"});

    return true;
}

bool yarp::dev::baseEstimatorV1::updateLogger()
{
    yarp::sig::Vector feet_contact_state;
    yarp::sig::Vector feet_contact_normal_forces;
    feet_contact_state.resize(2);
    feet_contact_normal_forces.resize(2);
    feet_contact_state(0) = m_biped_foot_contact_classifier->getLeftFootContactState();
    feet_contact_state(1) = m_biped_foot_contact_classifier->getRightFootContactState();
    feet_contact_normal_forces(0) = m_left_foot_contact_normal_force;
    feet_contact_normal_forces(1) = m_right_foot_contact_normal_force;
    m_logger->sendData(m_world_pose_base_in_R6, m_world_velocity_base, feet_contact_state,
                       feet_contact_normal_forces,
                       m_imu_attitude_estimate_as_rpy, m_world_velocity_base_from_imu);
    return true;
}


void yarp::dev::baseEstimatorV1::publish()
{
    publishFloatingBasePoseVelocity();
    publishContactState();
    publishFootPose();

    if (m_use_debug_ports)
    {
        publishFloatingBaseState();
        publishIMUAttitudeEstimatorStates();
        if (m_attitude_estimator_type == "qekf")
        {
            publishIMUAttitudeQEKFEstimates();
        }
    }
    publishTransform();
}

void yarp::dev::baseEstimatorV1::run()
{
    yarp::os::LockGuard guard(m_device_mutex);

    if(m_state != FilterFSM::RUNNING)
        return;

    // if (!m_device_initialized_correctly)
    // {
    //     calibFTSensorsStanding();
    // }

    if (readSensors(m_verbose))
    {
        getFeetCartesianWrenches();

        if (!m_device_initialized_correctly)
        {
            bool ok{false};
            ok = initializeLeggedOdometry();
            ok = initializeBipedFootContactClassifier();
            updateLeggedOdometry();
            if (m_attitude_estimator_type == "mahony")
            {
                ok = initializeIMUAttitudeEstimator();
            }
            else if (m_attitude_estimator_type == "qekf")
            {
                ok = initializeIMUAttitudeQEKF();
            }

            ok = alignIMUFrames();
            publish();
            if (m_dump_data)
            {
                initializeLogger();
            }
            m_previous_fixed_frame = m_current_fixed_frame;
            yInfo() << "Estimator initialized successfully";
            m_device_initialized_correctly = ok;
        }
        else
        {
            updateLeggedOdometry();
            if (m_attitude_estimator_type == "mahony")
            {
                updateIMUAttitudeEstimator();
            }
            else if (m_attitude_estimator_type == "qekf")
            {
                updateIMUAttitudeQEKF();
            }
            updateBasePoseWithIMUEstimates();
            updateFeetPoseWithIMUEstimates();
            updateBaseVelocity();
            //updateBaseVelocityWithIMU();
            publish();
            if (m_dump_data)
            {
                updateLogger();
            }
            m_previous_fixed_frame = m_current_fixed_frame;
        }
    }
    else
    {
        yError() << "floatingBaseEstimatorV1: " << " estimator will not return meaningful estimates, reading sensors failed";
    }

}

void yarp::dev::baseEstimatorV1::closeDevice()
{
    if (!m_imu_attitude_observer_estimated_state_port.isClosed())
    {
        m_imu_attitude_observer_estimated_state_port.close();
    }

    if (!m_imu_attitude_qekf_estimated_state_port.isClosed())
    {
        m_imu_attitude_qekf_estimated_state_port.close();
    }

    if (!m_floating_base_state_port.isClosed())
    {
        m_floating_base_state_port.close();
    }

    if (!m_floating_base_pose_port.isClosed())
    {
        m_floating_base_pose_port.close();
    }

    if (!m_contact_state_port.isClosed())
    {
        m_contact_state_port.close();
    }

    if (m_estimator_rpc_port.isOpen())
    {
        m_estimator_rpc_port.close();
    }

    m_device_initialized_correctly = false;
    m_remapped_control_board_interfaces.encs = nullptr;

    if (m_transform_broadcaster.isValid())
    {
        m_transform_broadcaster.close();
    }

    m_transform_interface = nullptr;

    if (m_wbd_is_open)
    {
        if (yarp::os::Network::isConnected(m_left_foot_cartesian_wrench_port_name, m_port_prefix + "/left_foot_cartesian_wrench:i"))
        {
            yarp::os::Network::disconnect(m_left_foot_cartesian_wrench_port_name, m_port_prefix + "/left_foot_cartesian_wrench:i");
        }

        if (yarp::os::Network::isConnected(m_right_foot_cartesian_wrench_port_name, m_port_prefix + "/right_foot_cartesian_wrench:i"))
        {
            yarp::os::Network::disconnect(m_right_foot_cartesian_wrench_port_name, m_port_prefix + "/right_foot_cartesian_wrench:i");
        }
    }
    if (!m_left_foot_cartesian_wrench_wbd_port.isClosed())
    {
        m_left_foot_cartesian_wrench_wbd_port.close();
    }

    if (!m_right_foot_cartesian_wrench_wbd_port.isClosed())
    {
        m_right_foot_cartesian_wrench_wbd_port.close();
    }

    if (m_dump_data)
    {
        m_logger->quit();
    }
}

bool yarp::dev::baseEstimatorV1::close()
{
    yarp::os::LockGuard guard(m_device_mutex);
    closeDevice();

    return true;
}

yarp::dev::baseEstimatorV1::~baseEstimatorV1()
{

}

/// RPC methods

bool yarp::dev::baseEstimatorV1::setMahonyKp(const double kp)
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_imu_attitude_observer->setGainkp(kp);
    return true;
}

bool yarp::dev::baseEstimatorV1::setMahonyKi(const double ki)
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_imu_attitude_observer->setGainki(ki);
    return true;
}

bool yarp::dev::baseEstimatorV1::setMahonyTimeStep(const double timestep)
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_imu_attitude_observer->setTimeStepInSeconds(timestep);
    return true;
}

bool yarp::dev::baseEstimatorV1::setContactSchmittThreshold(const double l_fz_break, const double l_fz_make,
                                                                        const double r_fz_break, const double r_fz_make)
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_biped_foot_contact_classifier->m_leftFootContactClassifier->m_contactSchmitt->setLowValueThreshold(l_fz_break);
    m_biped_foot_contact_classifier->m_leftFootContactClassifier->m_contactSchmitt->setHighValueThreshold(l_fz_make);
    m_biped_foot_contact_classifier->m_rightFootContactClassifier->m_contactSchmitt->setLowValueThreshold(r_fz_break);
    m_biped_foot_contact_classifier->m_rightFootContactClassifier->m_contactSchmitt->setHighValueThreshold(r_fz_make);

    m_biped_foot_contact_classifier->updateFootContactState(yarp::os::Time::now(), m_left_foot_contact_normal_force,
                                                            m_right_foot_contact_normal_force);
    return true;
}


std::string yarp::dev::baseEstimatorV1::getEstimationJointsList()
{
    yarp::os::LockGuard guard(m_device_mutex);
    std::stringstream ss;
    for (int i = 0; i < m_estimation_joint_names.size(); i++)
    {
        if (i != m_estimation_joint_names.size() - 1)
        {
            ss << m_estimation_joint_names[i] << ",";
        }
        else
        {
            ss << m_estimation_joint_names[i];
            break;
        }
    }

    return ss.str();
}

bool yarp::dev::baseEstimatorV1::setPrimaryFoot(const std::string& primary_foot)
{
    yarp::os::LockGuard guard(m_device_mutex);
    if (primary_foot == "right")
    {
        m_current_fixed_frame = "r_foot";
        m_biped_foot_contact_classifier->setPrimaryFoot(iDynTree::BipedFootContactClassifier::RIGHT_FOOT);
    }
    else if (primary_foot == "left")
    {
        m_current_fixed_frame = "l_foot";
        m_biped_foot_contact_classifier->setPrimaryFoot(iDynTree::BipedFootContactClassifier::LEFT_FOOT);
    }
    else
    {
        m_current_fixed_frame = "unknown";
    }

    m_previous_fixed_frame = "unknown";
    if (m_current_fixed_frame == "unknown")
    {
        return false;
    }
    m_legged_odometry->changeFixedFrame(m_current_fixed_frame);
    return true;
}

std::string yarp::dev::baseEstimatorV1::getRefFrameForWorld()
{
    yarp::os::LockGuard guard(m_device_mutex);
    return m_initial_reference_frame_for_world;
}

Pose6D yarp::dev::baseEstimatorV1::getRefPose6DForWorld()
{
    yarp::os::LockGuard guard(m_device_mutex);
    Pose6D ref_pose_world;
    ref_pose_world.x = m_initial_reference_frame_H_world.getPosition()(0);
    ref_pose_world.y = m_initial_reference_frame_H_world.getPosition()(1);
    ref_pose_world.z = m_initial_reference_frame_H_world.getPosition()(2);
    ref_pose_world.roll = m_initial_reference_frame_H_world.getRotation().asRPY()(0);
    ref_pose_world.pitch = m_initial_reference_frame_H_world.getRotation().asRPY()(1);
    ref_pose_world.yaw = m_initial_reference_frame_H_world.getRotation().asRPY()(2);
    return ref_pose_world;
}

bool yarp::dev::baseEstimatorV1::resetLeggedOdometry()
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_legged_odometry->updateKinematics(m_joint_positions);
    bool ok = m_legged_odometry->init(m_initial_fixed_frame, m_initial_reference_frame_for_world, m_initial_reference_frame_H_world);
    updateLeggedOdometry();
    return ok;
}

bool yarp::dev::baseEstimatorV1::resetIMU()
{
    yarp::os::LockGuard guard(m_device_mutex);
    bool ok{false};
    if (m_attitude_estimator_type == "qekf")
    {
        ok = initializeIMUAttitudeQEKF();
    }
    else if (m_attitude_estimator_type == "mahony")
    {
        ok = initializeIMUAttitudeEstimator();
    }

    ok = alignIMUFrames();
    updateBasePoseWithIMUEstimates();
   return ok;
}

bool yarp::dev::baseEstimatorV1::resetEstimator()
{
    bool ok{false};
    // stick to this order because alignIMUFrames depends on w_R_b estimate from LO
    ok = resetLeggedOdometry();
    ok = resetIMU();
    return ok;
}

bool yarp::dev::baseEstimatorV1::startFloatingBaseFilter()
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_state = FilterFSM::RUNNING;
    return true;
}

bool yarp::dev::baseEstimatorV1::resetLeggedOdometryWithRefFrame(const std::string& ref_frame,
                                                                      const double x, const double y, const double z,
                                                                      const double roll, const double pitch, const double yaw)
{
    yarp::os::LockGuard guard(m_device_mutex);
    m_initial_reference_frame_for_world = ref_frame;
    iDynTree::Position w_p_b(x, y ,z);
    iDynTree::Rotation w_R_b{iDynTree::Rotation::RPY(roll, pitch, yaw)};

    m_initial_reference_frame_H_world = iDynTree::Transform(w_R_b, w_p_b);
    m_legged_odometry->updateKinematics(m_joint_positions);
    m_initial_fixed_frame = ref_frame;
    yInfo() << "Initial fixed frame changed to " << m_initial_fixed_frame;
    bool ok = m_legged_odometry->init(m_initial_fixed_frame, m_initial_reference_frame_for_world, m_initial_reference_frame_H_world);
    yInfo() << "Initial ref_frame to world transform: " << m_initial_reference_frame_H_world.toString();
    if (ref_frame == "r_sole")
    {
        m_biped_foot_contact_classifier->setPrimaryFoot(iDynTree::BipedFootContactClassifier::RIGHT_FOOT);
    }
    else if (ref_frame == "l_sole")
    {
        m_biped_foot_contact_classifier->setPrimaryFoot(iDynTree::BipedFootContactClassifier::LEFT_FOOT);
    }
    m_legged_odometry->changeFixedFrame(ref_frame);

    updateLeggedOdometry();
    m_state = FilterFSM::RUNNING;
    return ok;
}

bool yarp::dev::baseEstimatorV1::resetEstimatorWithRefFrame(const std::string& ref_frame,
                                                    const double x, const double y, const double z,
                                                    const double roll, const double pitch, const double yaw)
{
    bool ok{false};
    // stick to this order because alignIMUFrames depends on w_R_b estimate from LO
    ok = resetLeggedOdometryWithRefFrame(ref_frame, x, y, z, roll, pitch, yaw);
    ok = resetIMU();
    return ok;
}

bool yarp::dev::baseEstimatorV1::setJointVelocityLPFCutoffFrequency(const double freq)
{
    m_joint_vel_filter_cutoff_freq = freq;
    m_joint_velocities_filter->setCutFrequency(freq);
    return true;
}

bool yarp::dev::baseEstimatorV1::useJointVelocityLPF(const bool flag)
{
    m_use_lpf = flag;
    return true;
}

