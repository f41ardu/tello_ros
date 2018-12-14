#include <asio.hpp>

#include "rclcpp/rclcpp.hpp"
#include "cv_bridge/cv_bridge.h"
#include "geometry_msgs/msg/twist.hpp"
#include "sensor_msgs/msg/camera_info.hpp"
#include "std_msgs/msg/empty.hpp"
#include "std_msgs/msg/string.hpp"
#include "tello_msgs/msg/flight_data.hpp"
#include "tello_msgs/msg/tello_response.hpp"
#include "tello_msgs/srv/tello_command.hpp"

#include "h264decoder.hpp"

using asio::ip::udp;

namespace tello_driver {

class CommandSocket;
class StateSocket;
class VideoSocket;

enum class SDK
{
  unknown,
  v1_3,
  v2_0
};

//=====================================================================================
// Tello driver implements Tello SDK 1.3 and 2.0
//
// Socket threads make these calls, which AFAIK are reentrant:
// rclcpp::Node::now()
// rclcpp::Node::count_subscribers()
// rclcpp::Node::get_logger()
// rclcpp::Publisher::get_topic_name()
// rclcpp::Publisher::publish()
//
// FastRTPS also uses asio, and there's already an asio::io_service in the rclcpp::Node
// process. This can cause a deadlock. We avoid this by pushing the asio calls to the
// TelloSocket instances, which run in their own threads. See:
// https://github.com/ros2/rmw_fastrtps/issues/176
//=====================================================================================

class TelloDriver : public rclcpp::Node
{
public:

  explicit TelloDriver();

  ~TelloDriver();

  void spin_once();

  // ROS publishers
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr image_pub_;
  rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr camera_info_pub_;
  rclcpp::Publisher<tello_msgs::msg::FlightData>::SharedPtr flight_data_pub_;
  rclcpp::Publisher<tello_msgs::msg::TelloResponse >::SharedPtr tello_response_pub_;

private:

  void spin_1s();

  void command_callback(
    const std::shared_ptr<rmw_request_id_t> request_header,
    const std::shared_ptr<tello_msgs::srv::TelloCommand::Request> request,
    std::shared_ptr<tello_msgs::srv::TelloCommand::Response> response);

  // Sockets
  std::unique_ptr<CommandSocket> command_socket_;
  std::unique_ptr<StateSocket> state_socket_;
  std::unique_ptr<VideoSocket> video_socket_;

  // ROS services
  rclcpp::Service<tello_msgs::srv::TelloCommand>::SharedPtr command_srv_;
};

//=====================================================================================
// Abstract socket
//=====================================================================================

class TelloSocket
{
public:

  TelloSocket(TelloDriver *driver, unsigned short port) : driver_(driver),
    socket_(io_service_, udp::endpoint(udp::v4(), port)) {}

  bool receiving();
  rclcpp::Time receive_time();
  virtual void timeout();

protected:

  void listen();
  virtual void process_packet(size_t r) = 0;

  TelloDriver *driver_;                 // Pointer to the driver node
  asio::io_service io_service_;         // Manages IO for this socket
  udp::socket socket_;                  // The socket
  std::thread thread_;                  // Each socket receives on it's own thread
  std::mutex mtx_;                      // All public calls must be guarded
  bool receiving_ = false;              // Are we receiving packets on this socket?
  rclcpp::Time receive_time_;           // Time of most recent receive
  std::vector<unsigned char> buffer_;   // Packet buffer
};

//=====================================================================================
// Command socket
//=====================================================================================

class CommandSocket : public TelloSocket
{
public:

  CommandSocket(TelloDriver *driver);

  void timeout() override;
  bool waiting();
  rclcpp::Time send_time();
  void initiate_command(std::string command, bool respond);

private:

  void process_packet(size_t r) override;
  void complete_command(uint8_t rc, std::string str);

#undef LOCAL_EMULATION
#ifdef LOCAL_EMULATION
  udp::endpoint remote_endpoint_{udp::v4(), 8889};
#else
  udp::endpoint remote_endpoint_{asio::ip::address_v4::from_string("192.168.10.1"), 8889};
#endif
  rclcpp::Time send_time_;  // Time of most recent send
  bool respond_;            // Send response on tello_response_pub_
  bool waiting_ = false;    // Are we waiting for a response?
};

//=====================================================================================
// State socket
//=====================================================================================

class StateSocket : public TelloSocket
{
public:

  StateSocket(TelloDriver *driver);

private:

  void process_packet(size_t r) override;

  SDK sdk_ = SDK::unknown;    // Tello SDK version
};

//=====================================================================================
// Video socket
//=====================================================================================

class VideoSocket : public TelloSocket
{
public:

  VideoSocket(TelloDriver *driver);

private:

  void process_packet(size_t r) override;
  void decode_frames();

  std::vector<unsigned char> seq_buffer_;   // Collect video packets into a larger sequence
  size_t seq_buffer_next_ = 0;              // Next available spot in the sequence buffer
  int seq_buffer_num_packets_ = 0;          // How many packets we've collected, for debugging

  H264Decoder decoder_;                     // Decodes h264
  ConverterRGB24 converter_;                // Converts pixels from YUV420P to BGR24
};

} // namespace tello_driver
