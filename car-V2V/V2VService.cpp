#include "V2VService.hpp"

int main(int argc, char **argv) {

    //extract command line arguments using cluon
    auto arguments = cluon::getCommandlineArguments(argc, argv);

    if (0 == arguments.count("cid") || 
    0 == arguments.count("cid2") || 
    0 == arguments.count("freq") || 
    0 == arguments.count("ip")) {
    std::cerr << "Leader Example: " << argv[0] << " --cid=140 --cid2=141 --freq=10 --ip=192.168.43.223 --leader=5 --counter=15" << std::endl;
    std::cerr << "Follower Example: " << argv[0] << " --cid=140 --cid2=141 --freq=10 --ip=192.168.43.223 --following=1  --leader=5 --counter=15" << std::endl;
    return -1;
  }


    //convert and assign values
    const std::string carIP = arguments["ip"];
    const std::string carID = "3";
    const uint16_t cid = (uint16_t) std::stoi(arguments["cid"]);
    //the delay that we need to calibrate the cars
    const int counter = std::stoi(arguments["counter"]);
    //cid2 used to send messages to decision layer
    const uint16_t cid2 = (uint16_t) std::stoi(arguments["cid2"]);
    //the group id that we want to follow
    const std::string leaderId = arguments["leader"];
    const uint16_t freq = (uint16_t) std::stoi(arguments["freq"]);
    //used to indicate that this car is in following mode
    const bool FOLLOWING{arguments.count("following") != 0};


    std::shared_ptr<V2VService> v2vService = std::make_shared<V2VService>(carIP, carID);
	std::cout << "leaderID: " << leaderId << "Following mode: " << FOLLOWING << std::endl;

    float speed = 0;
    float angle = 0;
    //get the speed and angle from the car
    cluon::OD4Session od4(cid,
                          [&speed, &angle](cluon::data::Envelope &&envelope) noexcept {
                              if (envelope.dataType() == opendlv::proxy::PedalPositionReading::ID()) {
                                  opendlv::proxy::PedalPositionReading pedalPositionReading =
                                          cluon::extractMessage<opendlv::proxy::PedalPositionReading>(std::move(envelope));
                                  speed = pedalPositionReading.position();
                              }
                              if (envelope.dataType() == opendlv::proxy::GroundSteeringReading::ID()) {
                                  opendlv::proxy::GroundSteeringReading groundSteeringReading =
                                          cluon::extractMessage<opendlv::proxy::GroundSteeringReading>(std::move(envelope));
                                  angle = groundSteeringReading.groundSteering();
                              }
                          });
    //od4 session used to send commands to decision layer
    cluon::OD4Session internal{cid2};
    static int count = 0;
    static float old_angle = 1;

    auto communication{[&v2vService, &speed, &angle, &cid, &FOLLOWING, &leaderId, &internal, &counter]() -> bool {

        //sending ann presence messages until leader or follower ip are empty
        v2vService->announcePresence();
        //getting the ip with the leaderID
        std::string ip = v2vService->getIPfromID(leaderId);

        //using command line arguments to check whether car is set up to follow or lead
        if(FOLLOWING){
            //after announcing presence sending a follow request to the ip with the predefined ID in arguments
            v2vService->followRequest(ip);
	        v2vService->followerStatus();

            opendlv::proxy::GroundSteeringReading msgAngle;
            opendlv::proxy::PedalPositionReading msgSpeed;

            //just follow messages queue generated from UDP inbox
            if(!v2vService->commandQ.empty()){

                float decrease = 0.01;
                //saving the leader angle and speed
                float leader_angle = v2vService->commandQ.front().steeringAngle();
                float leader_speed = v2vService->commandQ.front().speed();

                if(leader_speed != 0) leader_speed -= decrease;
                    
                //deleting last read message
                v2vService->commandQ.pop();

                //sending messages to decision layer
                msgSpeed.position(leader_speed);
                internal.send(msgSpeed);
                
		//counter logic for the delay of the steering angle
                if(leader_angle != old_angle){
                    count++;
                }
                if(count >= counter){
                    msgAngle.groundSteering(leader_angle);
                    internal.send(msgAngle);
                    old_angle = leader_angle;
                    count = 0;
                }

            }else{
                //change the speed to 0
                msgSpeed.position(0.0);
                internal.send(msgSpeed);
            }

        }else{
            v2vService->leaderStatus(speed, angle, 5);
            std::cout << "Speed: " << speed <<"Angle: " << angle << std::endl;
        }
        return true;
    }};
    od4.timeTrigger(freq, communication);
}

/**
 * Implementation of the V2VService class as declared in V2VService.hpp
 */
V2VService::V2VService(std::string carIP, std::string groupID) {
    /*
     * The broadcast field contains a reference to the broadcast channel which is an OD4Session. This is where
     * AnnouncePresence messages will be received.
     */
    carIp = carIP;
    groupId = groupID;

    broadcast =

            std::make_shared<cluon::OD4Session>(BROADCAST_CHANNEL,
                                                [this](cluon::data::Envelope &&envelope) noexcept {
                                                    std::cout << "[OD4] ";
                                                    switch (envelope.dataType()) {
                                                        case ANNOUNCE_PRESENCE: {
                                                            AnnouncePresence ap = cluon::extractMessage<AnnouncePresence>(std::move(envelope));
                                                            std::cout << "received 'AnnouncePresence' from '"
                                                                      << ap.vehicleIp() << "', GroupID '"
                                                                      << ap.groupId() << "'!" << std::endl;

                                                            presentCars[ap.groupId()] = ap.vehicleIp();

                                                            break;
                                                        }
                                                        default: std::cout << "¯\\_(ツ)_/¯" << std::endl;
                                                    }
                                                });


    /*
     * Each car declares an incoming UDPReceiver for messages directed at them specifically. This is where messages
     * such as FollowRequest, FollowResponse, StopFollow, etc. are received.
     */
    incoming =
            std::make_shared<cluon::UDPReceiver>("0.0.0.0", DEFAULT_PORT,
                                                 [this](std::string &&data, std::string &&sender, std::chrono::system_clock::time_point &&ts) noexcept {
                                                     std::cout << "[UDP] ";
                                                     std::pair<int16_t, std::string> msg = extract(data);

                                                     switch (msg.first) {
                                                         case FOLLOW_REQUEST: {
                                                             FollowRequest followRequest = decode<FollowRequest>(msg.second);
                                                             std::cout << "received '" << followRequest.LongName()
                                                                       << "' from '" << sender << "'!" << std::endl;

                                                             // After receiving a FollowRequest, check first if there is currently no car already following.
                                                             if (followerIp.empty()) {
                                                                 unsigned long len = sender.find(':');    // If no, add the requester to known follower slot
                                                                 followerIp = sender.substr(0, len);      // and establish a sending channel.
                                                                 toFollower = std::make_shared<cluon::UDPSender>(followerIp, DEFAULT_PORT);
                                                                 followResponse();
                                                             }
                                                             break;
                                                         }
                                                         case FOLLOW_RESPONSE: {
                                                             FollowResponse followResponse = decode<FollowResponse>(msg.second);
                                                             std::cout << "received '" << followResponse.LongName()
                                                                       << "' from '" << sender << "'!" << std::endl;
                                                             break;
                                                         }
                                                         case STOP_FOLLOW: {
                                                             StopFollow stopFollow = decode<StopFollow>(msg.second);
                                                             std::cout << "received '" << stopFollow.LongName()
                                                                       << "' from '" << sender << "'!" << std::endl;

                                                             // Clear either follower or leader slot, depending on current role.
                                                             unsigned long len = sender.find(':');
                                                             if (sender.substr(0, len) == followerIp) {
                                                                 followerIp = "";
                                                                 toFollower.reset();
                                                             }
                                                             else if (sender.substr(0, len) == leaderIp) {
                                                                 leaderIp = "";
                                                                 toLeader.reset();
                                                             }
                                                             break;
                                                         }
                                                         case FOLLOWER_STATUS: {
                                                             FollowerStatus followerStatus = decode<FollowerStatus>(msg.second);
                                                             std::cout << "received '" << followerStatus.LongName()
                                                                       << "' from '" << sender << "'!" << std::endl;


                                                             break;
                                                         }
                                                         case LEADER_STATUS: {
                                                             LeaderStatus leaderStatus = decode<LeaderStatus>(msg.second);
                                                             std::cout << "received '" << leaderStatus.LongName()
                                                                       << "' from '" << sender << " Speed: "
                                                                       << leaderStatus.speed() << " Angle: "
                                                                       << leaderStatus.steeringAngle() << std::endl;

                                                            // float leader_angle = leaderStatus.steeringAngle();
                                                            // float leader_speed = leaderStatus.speed();

                                                             commandQ.push(leaderStatus);

                                                             break;
                                                         }
                                                         default: std::cout << "¯\\_(ツ)_/¯" << std::endl;
                                                     }
                                                 });
}

/**
 * This function sends an AnnouncePresence (id = 1001) message on the broadcast channel. It will contain information
 * about the sending vehicle, including: IP, port and the group identifier.
 */
void V2VService::announcePresence() {
    if (!followerIp.empty() || !leaderIp.empty()) return;
    AnnouncePresence announcePresence;
    announcePresence.vehicleIp(carIp);
    announcePresence.groupId(groupId);
    broadcast->send(announcePresence);
}

/**
 * This function sends a FollowRequest (id = 1002) message to the IP address specified by the parameter vehicleIp. And
 * sets the current leaderIp field of the sending vehicle to that of the target of the request.
 *
 * @param vehicleIp - IP of the target for the FollowRequest
 */
void V2VService::followRequest(std::string vehicleIp) {
    if (!leaderIp.empty()) return;
    std::cout << "SENDING FOLLOW REQUEST TO: " << vehicleIp << std::endl;
    leaderIp = vehicleIp;
    toLeader = std::make_shared<cluon::UDPSender>(leaderIp, DEFAULT_PORT);
    FollowRequest followRequest;
    toLeader->send(encode(followRequest));
}

/**
 * This function send a FollowResponse (id = 1003) message and is sent in response to a FollowRequest (id = 1002).
 * This message will contain the NTP server IP for time synchronization between the target and the sender.
 */
void V2VService::followResponse() {
    if (followerIp.empty()) return;
    FollowResponse followResponse;
    toFollower->send(encode(followResponse));
}

/**
 * This function sends a StopFollow (id = 1004) request on the ip address of the parameter vehicleIp. If the IP address is neither
 * that of the follower nor the leader, this function ends without sending the request message.
 *
 * @param vehicleIp - IP of the target for the request
 */
void V2VService::stopFollow(std::string vehicleIp) {
    StopFollow stopFollow;
    if (vehicleIp == leaderIp) {
        toLeader->send(encode(stopFollow));
        leaderIp = "";
        toLeader.reset();
    }
    if (vehicleIp == followerIp) {
        toFollower->send(encode(stopFollow));
        followerIp = "";
        toFollower.reset();
    }
}

/**
 * This function sends a FollowerStatus (id = 3001) message on the leader channel.
 *
 * @param speed - current velocity
 * @param steeringAngle - current steering angle
 * @param distanceFront - distance to nearest object in front of the car sending the status message
 * @param distanceTraveled - distance traveled since last reading
 */
void V2VService::followerStatus() {
    if (leaderIp.empty()) return;
    FollowerStatus followerStatus;
    toLeader->send(encode(followerStatus));
}

/**
 * This function sends a LeaderStatus (id = 2001) message on the follower channel.
 *
 * @param speed - current velocity
 * @param steeringAngle - current steering angle
 * @param distanceTraveled - distance traveled since last reading
 */
void V2VService::leaderStatus(float speed, float steeringAngle, uint8_t distanceTraveled) {
    if (followerIp.empty()) return;
    LeaderStatus leaderStatus;
    leaderStatus.timestamp(getTime());
    leaderStatus.speed(speed);
    leaderStatus.steeringAngle(steeringAngle);
    leaderStatus.distanceTraveled(distanceTraveled);
    toFollower->send(encode(leaderStatus));
}

/**
 * Gets the current time.
 *
 * @return current time in milliseconds
 */
uint32_t V2VService::getTime() {
    timeval now;
    gettimeofday(&now, nullptr);
    return (uint32_t ) now.tv_usec / 1000;
}

/**
 * The extraction function is used to extract the message ID and message data into a pair.
 *
 * @param data - message data to extract header and data from
 * @return pair consisting of the message ID (extracted from the header) and the message data
 */
std::pair<int16_t, std::string> V2VService::extract(std::string data) {
    if (data.length() < 10) return std::pair<int16_t, std::string>(-1, "");
    int id, len;
    std::stringstream ssId(data.substr(0, 4));
    std::stringstream ssLen(data.substr(4, 10));
    ssId >> std::hex >> id;
    ssLen >> std::hex >> len;
    return std::pair<int16_t, std::string> (
            data.length() -10 == len ? id : -1,
            data.substr(10, data.length() -10)
    );
};

/**
 * Generic encode function used to encode a message before it is sent.
 *
 * @tparam T - generic message type
 * @param msg - message to encode
 * @return encoded message
 */
template <class T>
std::string V2VService::encode(T msg) {
    cluon::ToProtoVisitor v;
    msg.accept(v);
    std::stringstream buff;
    buff << std::hex << std::setfill('0')
         << std::setw(4) << msg.ID()
         << std::setw(6) << v.encodedData().length()
         << v.encodedData();
    return buff.str();
}

/**
 * Generic decode function used to decode an incoming message.
 *
 * @tparam T - generic message type
 * @param data - encoded message data
 * @return decoded message
 */
template <class T>
T V2VService::decode(std::string data) {
    std::stringstream buff(data);
    cluon::FromProtoVisitor v;
    v.decodeFrom(buff);
    T tmp = T();
    tmp.accept(v);
    return tmp;
}

/**
 * Function that retrieves the ip from the groupID map where the key is the ID
 */
std::string V2VService::getIPfromID(std::string id){
    //std::cout << "IP IS" << presentCars[id] << std::endl;
    return presentCars[id];
}

