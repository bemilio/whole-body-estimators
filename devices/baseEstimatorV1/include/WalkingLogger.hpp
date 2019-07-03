/**
 * @file WalkingLogger.hpp
 * @authors Giulio Romualdi <giulio.romualdi@iit.it>
 * @copyright 2018 iCub Facility - Istituto Italiano di Tecnologia
 *            Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 * @date 2018
 */

#ifndef WALKING_LOGGER_HPP
#define WALKING_LOGGER_HPP

// YARP
#include <yarp/os/Searchable.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/os/RpcClient.h>
#include <yarp/sig/Vector.h>

class WalkingLogger
{
    yarp::os::BufferedPort<yarp::sig::Vector> m_dataPort; /**< Data logger port. */
    yarp::os::RpcClient m_rpcPort; /**< RPC data logger port. */

public:

    /**
     * Configure
     * @param config yarp searchable configuration variable;
     * @param name is the name of the module.
     * @return true/false in case of success/failure.
     */
    bool configure(const yarp::os::Searchable& config, const std::string& name);

    /**
     * Start record.
     * @param strings head of the logger file
     * @return true/false in case of success/failure.
     */
    bool startRecord(const std::initializer_list<std::string>& strings);

    /**
     * Quit the logger.
     */
    void quit();

    /**
     * Send data to the logger.
     * @param args all the vector containing the data that will be sent.
     */
    template <typename... Args>
    void sendData(const Args&... args);
};

#include "WalkingLogger.tpp"

#endif
