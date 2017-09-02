#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// starting lane
int lane = 1;

// target reference velocity in mph
// initially, to avoid the cold start issue, this is set to 0
// an acceleration of 5m/s^2 is applied within the sensor fusion code section
double ref_vel = 49.5;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, vector<double> maps_x, vector<double> maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2( (map_y-y),(map_x-x) );

	double angle = abs(theta-heading);

	if(angle > pi()/4)
	{
		closestWaypoint++;
	}

	return closestWaypoint;

}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, vector<double> maps_s, vector<double> maps_x, vector<double> maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

void generateNewTrajectory(double car_x, double car_y, double car_yaw, vector<double> previous_path_x,
                          vector<double> previous_path_y, vector<double> map_waypoints_s,
                          vector<double> map_waypoints_x, vector<double> map_waypoints_y,
                          int lane, double car_s, vector<double> &next_x_vals, vector<double> &next_y_vals)
{
  // list of widely spaced waypoints to be intrepolated by splines
  vector<double> ptsx, ptsy;

  // reference states
  double ref_x = car_x;
  double ref_y = car_y;
  double ref_x_prev, ref_y_prev;
  double ref_yaw = deg2rad(car_yaw);
  int path_size = previous_path_x.size();

  // USE SPLINE TO CREATE PATH
  // use car as starting reference if the previous path is empty
  if (path_size<2)
  {
    // extrapolate backwards one point
    ref_x_prev = ref_x - cos(car_yaw);
    ref_y_prev = ref_y - sin(car_yaw);
  }
  else // use two points from previous path
  {
    ref_x = previous_path_x[path_size-1];
    ref_y = previous_path_y[path_size-1];

    ref_x_prev = previous_path_x[path_size-2];
    ref_y_prev = previous_path_y[path_size-2];

    ref_yaw = atan2(ref_y-ref_y_prev, ref_x-ref_x_prev);
  }
  ptsx.push_back(ref_x_prev);
  ptsx.push_back(ref_x);

  ptsy.push_back(ref_y_prev);
  ptsy.push_back(ref_y);

  // use Frenet coordinates to add evenly spaced way-points
  vector<double> new_wp0 = getXY(car_s+30, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
  vector<double> new_wp1 = getXY(car_s+60, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
  vector<double> new_wp2 = getXY(car_s+90, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);

  ptsx.push_back(new_wp0[0]);
  ptsx.push_back(new_wp1[0]);
  ptsx.push_back(new_wp2[0]);

  ptsy.push_back(new_wp0[1]);
  ptsy.push_back(new_wp1[1]);
  ptsy.push_back(new_wp2[1]);

  // transform to local car coordinates
  double shift_x, shift_y;
  for (int i=0; i<ptsx.size(); i++)
  {
    // rotation shift
    shift_x = ptsx[i]-ref_x;
    shift_y = ptsy[i]-ref_y;

    ptsx[i] = shift_x*cos(-ref_yaw) - shift_y*sin(-ref_yaw);
    ptsy[i] = shift_x*sin(-ref_yaw) + shift_y*cos(-ref_yaw);
  }

  // add previous path point to the new path
  for (int i=0; i<path_size; i++)
  {
    next_x_vals.push_back(previous_path_x[i]);
    next_y_vals.push_back(previous_path_y[i]);
  }

  // allocate spline and set points
  tk::spline s;
  s.set_points(ptsx, ptsy);

  // compute spline points spacing to respect speed limit
  double target_x = 30;
  double target_y = s(target_x);
  double target_distance = sqrt(pow(target_x, 2)+ pow(target_y, 2));
  double x_add_on = 0;

  // fill the rest of the path
  double N, x_point, y_point, x_ref, y_ref;
  for (int i=1; i<=50-path_size; i++)
  {
    N = target_distance/(.02*ref_vel/2.24);
    x_point = x_add_on+target_x/N;
    y_point = s(x_point);

    x_add_on = x_point;

    x_ref = x_point;
    y_ref = y_point;

    // rotate back
    x_point = x_ref*cos(ref_yaw) - y_ref*sin(ref_yaw);
    y_point = x_ref*sin(ref_yaw) + y_ref*cos(ref_yaw);

    x_point += ref_x;
    y_point += ref_y;

    next_x_vals.push_back(x_point);
    next_y_vals.push_back(y_point);
  }
}

void lookAhead(vector<vector<double> > sensor_fusion, int lane, int path_size,
              double car_s, bool &too_close, double car_speed, double &ref_vel)
{
  double other_car_d;
  for (int i=0; i<sensor_fusion.size(); i++)
  {
    other_car_d = sensor_fusion[i][6];
    // if a car is sensed in my lane
    if (other_car_d < 2 + 4*lane + 2 && other_car_d > 2 + 4*lane - 2)
    {
      // compute other car velocity in m/s
      double vx = sensor_fusion[i][3]; // retrieve velocity components
      double vy = sensor_fusion[i][4];
      double other_car_speed = sqrt(pow(vx, 2)+pow(vy, 2)); // v magnitude

      // check the distance from me
      double other_car_s = sensor_fusion[i][5];

      // compensate latency
      other_car_s += ((double)path_size*0.02*other_car_speed);

      // if it is too close
      double car_distance = other_car_s - car_s;
      if (other_car_s > car_s && car_distance < 30)
      {
        // danger
        too_close = true;

        // decrease velocity if it is slower than me
        double other_car_speed_mph = other_car_speed*2.23694; // convert to mph
        if (other_car_speed_mph < car_speed - 0.112)
        {
          double deceleration; // parabolic deceleration
          deceleration = car_distance*(car_distance/900 - 1/15) + 1;

          ref_vel -= 0.1*deceleration;
          cout << other_car_speed_mph << " " << car_speed << endl;
        }
      }
    }
  }
}

bool check_lane(vector<vector<double> > sensor_fusion, int lane, double car_s, int path_size)
{
  double vx, vy, check_speed, check_car_s, d;
  bool danger = false;
  for (int i=0; i<sensor_fusion.size(); i++)
  {
    d = sensor_fusion[i][6]; // retrieve d
    if (d < 2 + 4*lane + 2 && d > 2 + 4*lane -2) // a car in that lane
    {
      vx = sensor_fusion[i][3]; // retrieve velocity components
      vy = sensor_fusion[i][4];
      check_speed = sqrt(pow(vx, 2)+pow(vy, 2)); // v magnitude

      check_car_s = sensor_fusion[i][5]; // how far is this car?
      // check_car_s += ((double)path_size*0.02*check_speed);

      if (check_car_s - car_s < 25 and check_car_s > car_s)
      {
        danger = true;
      }
    }
  }
  return danger;
}

int main() {

  //
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);

        string event = j[0].get<string>();

        if (event == "telemetry") {
          // j[1] is the data JSON object

        	// Main car's localization Data
          	double car_x = j[1]["x"];
          	double car_y = j[1]["y"];
          	double car_s = j[1]["s"];
          	double car_d = j[1]["d"];
          	double car_yaw = j[1]["yaw"];
          	double car_speed = j[1]["speed"];

          	// Previous path data given to the Planner
          	auto previous_path_x = j[1]["previous_path_x"];
          	auto previous_path_y = j[1]["previous_path_y"];
          	// Previous path's end s and d values
          	double end_path_s = j[1]["end_path_s"];
          	double end_path_d = j[1]["end_path_d"];

          	// Sensor Fusion Data, a list of all other cars on the same side of the road.
          	auto sensor_fusion = j[1]["sensor_fusion"];

          	json msgJson;

          	vector<double> next_x_vals;
          	vector<double> next_y_vals;

          	// TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds

            int path_size = previous_path_x.size();
            if (path_size>0)
            {
              car_s = end_path_s;
            }

            // SENSOR FUSION
            bool too_close = false;
            lookAhead(sensor_fusion, lane, path_size, car_s, too_close, car_speed, ref_vel);

            if (too_close == false && ref_vel < 49.75)
            {
              ref_vel += 0.224;
            }



            // bool too_close = false;
            //
            // float d; // other car d, s, and velocity
            // double vx, vy, check_speed, check_car_s;
            // double x0, y0, x1, y1, d0, s1, d1, vs, vd;
            // for (int i=0; i<sensor_fusion.size(); i++)
            // {
            //   d = sensor_fusion[i][6]; // retrieve d
            //   if (d < 2 + 4*lane + 2 && d > 2 + 4*lane -2) // another car in my lane
            //   {
            //     vx = sensor_fusion[i][3]; // retrieve velocity components
            //     vy = sensor_fusion[i][4];
            //     check_speed = sqrt(pow(vx, 2)+pow(vy, 2)); // v magnitude
            //
            //     check_car_s = sensor_fusion[i][5]; // how far is this car?
            //     check_car_s += ((double)path_size*0.02*check_speed);
            //
            //     // if the car ahead is too close (less than 30 meters)
            //     if (check_car_s > car_s && check_car_s - car_s < 15)
            //     {
            //       ref_vel -= 0.224;
            //     }
            //     else if (check_car_s > car_s && check_car_s - car_s < 30)
            //     {
            //       too_close = true; // change proximity flag
            //
            //       if (lane == 0)
            //       {
            //         bool right = check_lane(sensor_fusion, lane+1, car_s, path_size);
            //         if (right == false)
            //         {
            //           // ref_vel += 0.224;
            //           lane += 1;
            //         }
            //         else if (ref_vel > check_speed)
            //         {
            //           ref_vel -= 0.06;
            //         }
            //       }
            //       else if (lane == 1)
            //       {
            //         bool left = check_lane(sensor_fusion, lane-1, car_s, path_size);
            //         bool right = check_lane(sensor_fusion, lane+1, car_s, path_size);
            //         if (left == false)
            //         {
            //           // ref_vel += 0.224;
            //           lane -= 1;
            //         }
            //         else if (right == false)
            //         {
            //           // ref_vel += 0.224;
            //           lane += 1;
            //         }
            //         else if (ref_vel > check_speed)
            //         {
            //           ref_vel -= 0.06;
            //         }
            //       }
            //       else
            //       {
            //         bool left = check_lane(sensor_fusion, lane-1, car_s, path_size);
            //         if (left == false)
            //         {
            //           // ref_vel += 0.224;
            //           lane -= 1;
            //         }
            //         else if (ref_vel > check_speed)
            //         {
            //           ref_vel -= 0.06;
            //         }
            //       }
            //     }
            //   }
            // }
            //
            //
            // // decrease or increase velocity depending on reference velocity and proximity flag
            // if (too_close)
            // {
            //   ref_vel -= 0.06; // 5 m/s^2
            // }
            // else if (ref_vel<49.5)
            // {
            //   ref_vel += 0.224;
            // }

            generateNewTrajectory(car_x, car_y, car_yaw, previous_path_x,
                          previous_path_y, map_waypoints_s,
                          map_waypoints_x, map_waypoints_y,
                          lane, car_s, next_x_vals, next_y_vals);

            //---------------------------------------------------------------------

          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;

          	auto msg = "42[\"control\","+ msgJson.dump()+"]";

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);

        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
