#include <ros/ros.h>
#include <limits>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/core/core.hpp>
#include <iostream>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <math.h>
#include <sensor_msgs/Image.h>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/core/eigen.hpp>
#include <iostream>
#include <chrono> 

#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

//#include "vision_matrice100/DurationStamped.h"
#include "../include/utils_lib.h"

#include <nav_msgs/Odometry.h>
#include <tf/transform_datatypes.h>
#include <thread>


typedef std::chrono::high_resolution_clock Clock;

using namespace Eigen;
using namespace sensor_msgs;
using namespace std;
using namespace message_filters;

//Publisher
ros::Publisher pub_img_out;   // publish image mask
ros::Publisher panel_LinesFeatures_pub; // validacion de color
ros::Publisher lines_features_pub; // publico los parametros encontrado de las lineas izquierda y derecha
ros::Publisher time_pub;          // tiempo de ejecucion


// topics a suscribirse del nodo
std::string rgb_Topic   = "/camera/color/image_raw";
std::string depth_Topic = "/camera/aligned_depth_to_color/image_raw";
std::string odom_Topic  = "/dji_sdk/odometry";


float area_filter = 600.0; // 800 para real, 40 para simulado (tambien se modifica en el launch)
bool real_sim = true;  // (real == True) (sim == False)
bool kalman_bool = true; // variable para activar o desactivar el filtro de kalman

float hsv_v = 200;
float hsv_s = 100;


// Inicialización de y matrices
Eigen::MatrixXd A(4, 4), H(4, 4), Q(4,4), R(4, 4), K(4, 4), P_left(4,4), P_right(4,4);;
Eigen::VectorXd x_estimate_l(4), x_estimate_r(4);
Eigen::VectorXd curr_l(4), curr_r(4);
bool inicio_kalman = true;

std::tuple<Eigen::VectorXd, Eigen::MatrixXd> kalman_filter(Eigen::VectorXd curr_state, Eigen::VectorXd x_estimate_1, Eigen::MatrixXd P_k_1, Eigen::VectorXd vel_body){
  Eigen::MatrixXd x_estimate (4,1);
  Eigen::MatrixXd x_es (4,1);
  Eigen::MatrixXd P(4,4); 
  Eigen::MatrixXd P_curr(4,4); 
  Eigen::MatrixXd B(4,6);

  A <<  1.0009, -0.0000,  0.0006, -0.0008
      , 0.0055,  0.9999, -0.0052,  0.0035
      , 0.0140,  0.0002,  0.9862,  0.0092
      ,-0.0150,  0.0001,  0.0261,  0.9781;
  
  B << -0.9638   , 8.9483   , 0.0896   ,-0.1520  , 0.0094   , 0.0040
      , 0.7797   ,-6.4889   , 0.0767   ,-0.3674  , 0.0065   ,-0.1636
      , 0.3306   ,-4.1860   , 0.6446   , 0.0237  ,-0.0293   , 0.0055
      ,-1.7365   ,16.9488   ,-0.6440   ,-0.0352  , 0.0546   , 0.0209;

  H << 1, 0, 0, 0
      ,0, 1, 0, 0
      ,0, 0, 1, 0
      ,0, 0, 0, 1;

  Q << 1, 0, 0, 0
      ,0, 1, 0, 0
      ,0, 0, 1, 0
      ,0, 0, 0, 1 ;

  R << 10, 0, 0, 0
      ,0, 10, 0, 0
      ,0, 0, 100, 0
      ,0, 0, 0, 100 ;

  // filtro de Kalman
  x_es = A * x_estimate_1 + B * vel_body; // aqui debo sumar B
  P = A * P_k_1 * A.transpose() + 100.0* Q;  

  // // Estimacion
  Eigen::MatrixXd var_m(4,4) ;
  var_m = H * P * H.transpose() + 0.01 *R;

  K = P * H.transpose() * var_m.inverse();
  x_estimate = x_es + K * (curr_state - H * x_es);

  // actualizacion de P
  P_curr = (Eigen::MatrixXd::Identity(4, 4) - K * H) * P;

return std::make_tuple(x_estimate,P_curr);
}

// variables globales que van a ir en la funcion rgb_filter

cv::Mat gray_image_filter;
std::vector<cv::Point>  contour_left;
std::vector<cv::Point> contour_right;

void rgb_filter(cv::Mat rgb_image, cv::Scalar lowerWhite, cv::Scalar upperWhite ){
  cv::Mat hsvImage;
  cv::cvtColor(rgb_image, hsvImage, cv::COLOR_BGR2HSV);

  // Binarizar la imagen utilizando el rango de colores blanco
  cv::Mat binaryImage;
  cv::inRange(hsvImage, lowerWhite, upperWhite, binaryImage);

  // Buscar los contornos en la imagen binaria
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(binaryImage, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);  
 
  double areaThreshold = area_filter; // Ajusta el umbral de área según tus necesidades
  // Crear una imagen de salida para mostrar los contornos filtrados
  cv::Mat outputImage = cv::Mat::zeros(binaryImage.size(), CV_8UC3);

  // Filtrar los contornos más pequeños
  std::vector<cv::Vec4f> lines_input;
  for (size_t i = 0; i < contours.size(); i++)
  {
      double area = cv::contourArea(contours[i]);
      if (area > areaThreshold)
      {
          // Dibujar los contornos filtrados en la imagen de salida
          cv::Scalar color(255, 255, 255); // Color verde
          cv::drawContours(outputImage, contours, static_cast<int>(i), color, 2) ;
      }
  }

  cv::cvtColor(outputImage, gray_image_filter, CV_BGR2GRAY);  
  // dilatacion vertical 
  cv::Mat kernel_dil = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 50)); // Kernel de 1x21 para la dilatación vertical
  cv::dilate(gray_image_filter, gray_image_filter, kernel_dil);

    // Recorrer la imagen por filas y columnas
  for (int i = 100; i < rgb_image.rows-100; i++) { // filas
      bool flag_left = false;
      bool flag_rigth = false;
      for (int j = 1; j < rgb_image.cols/2; j++) { // columnas
          // Obtener el valor del pixel en (i, j)

          int pixel_left  = static_cast<int>(gray_image_filter.at<uchar>(i, j));
          int pixel_right = static_cast<int>(gray_image_filter.at<uchar>(i, rgb_image.cols-j));

          if (pixel_left==255 && flag_left == false){
            contour_left.push_back(cv::Point(j, i));
            flag_left = true;
          }
          if (pixel_right==255 && flag_rigth == false){
            contour_right.push_back(cv::Point(rgb_image.cols-j, i));
            flag_rigth = true;
          }
          if (flag_rigth && flag_left) // al ya tener las dos banderas en true se rompe el lazo for
            break;
          
      }
  }

}

//void callback(const CompressedImageConstPtr& in_rgb, const CompressedImageConstPtr& in_depth)
//void callback(const ImageConstPtr& in_rgb, const ImageConstPtr& in_depth)
void callback(const ImageConstPtr& in_rgb, const ImageConstPtr& in_depth, const nav_msgs::Odometry::ConstPtr& odom_msg)
{

  auto t1 = Clock::now();
  ros::Time start_time = ros::Time::now();
  Eigen::VectorXd  vel_world(6), vel_body(6);
  ///////////////////////Odometria del dron

  // Obtener la orientación del mensaje de odometría como un objeto Quaternion
  tf::Quaternion q;
  tf::quaternionMsgToTF(odom_msg->pose.pose.orientation, q);

  // Convertir el quaternion a una matriz de rotación
  tf::Matrix3x3 m(q);
  // Convertir la matriz de rotación a una matriz de transformación de la librería Eigen

  Eigen::Matrix3f  R_odom = Eigen::Matrix3f::Identity();

  geometry_msgs::Twist velocity = odom_msg->twist.twist;


  vel_world <<  velocity.linear.x, velocity.linear.y, velocity.linear.z, velocity.angular.x, velocity.angular.y, velocity.angular.z;
  
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            R_odom(i, j) = m[i][j];
        }
    }
    // T_odom(0, 3) = odom_msg->pose.pose.position.x;
    // T_odom(1, 3) = odom_msg->pose.pose.position.y;
    // T_odom(2, 3) = odom_msg->pose.pose.position.z;

     Eigen::MatrixXd Rt(6,6);
     Rt << R_odom(0), R_odom(1), R_odom(2) ,0        ,0         ,0
          ,R_odom(3), R_odom(4), R_odom(5) ,0        ,0         ,0
          ,R_odom(6), R_odom(7), R_odom(8) ,0        ,0         ,0
          ,0        ,0         , 0         ,R_odom(0), R_odom(1), R_odom(2) 
          ,0        ,0         , 0         ,R_odom(3), R_odom(4), R_odom(5) 
          ,0        ,0         , 0         ,R_odom(6), R_odom(7), R_odom(8) ;

  vel_body = Rt.transpose() * vel_world;
  //vel_body <<  0,0,0,0,0,0;


  /////////////////////////////////////////////

  cv_bridge::CvImagePtr cv_rgbCam, cv_depthCam;
  try
  {
    cv_rgbCam =   cv_bridge::toCvCopy(in_rgb, sensor_msgs::image_encodings::BGR8);  // imagen de profundida de la camara 
    cv_depthCam = cv_bridge::toCvCopy(in_depth, sensor_msgs::image_encodings::TYPE_16UC1);
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }


  cv::Mat rgb_image    =    cv_rgbCam->image; 
  cv::Mat Image_lines =  cv_rgbCam->image; // imange para plotear las linas

  // Obtener las dimensiones de la imagen
  int imageHeight = rgb_image.rows; // rows
  int imageWidth  = rgb_image.cols; // cols


  /////////////////////////////////////////////////// Inicio  de Filtrado de la imagen a COLOR //////////////////////////////////////////
 
  // Definir el rango de colores blanco en HSV
  cv::Scalar lowerWhite;
  cv::Scalar upperWhite;

  if (real_sim) // bandera para escoger entre la simulacion y el real (real == True) (sim == False)
  {
    lowerWhite = cv::Scalar(0, 0, hsv_v);  // Umbral inferior para blanco
    upperWhite = cv::Scalar(180, hsv_s, 255);  // Umbral superior para blanco
  }
  else
  {
    lowerWhite = cv::Scalar(0, 0, 200);  // Umbral inferior para blanco
    upperWhite = cv::Scalar(180, 30, 255);  // Umbral superior para blanco
  }
  cv::Mat image_in = rgb_image.clone();
  std::thread thread_1(rgb_filter, std::ref(image_in),lowerWhite,upperWhite);

  ////////////////////////////////////////////////////////// Fin del filtrado de la imagen a color //////////////////////////////////////////////////////
  
  
  cv::Mat mono_resultImage   = cv::Mat::zeros(imageHeight, imageWidth, CV_8UC1);
  cv::Mat outputImage_points = cv::Mat::zeros(imageHeight, imageWidth, CV_8UC3);

 
  //cv::drawContours(outputImage_points, std::vector<std::vector<cv::Point>>{contour_left}, 0, cv::Scalar(0, 255, 0), 1);
  // for (const auto& point : contour_left) 
  //       cv::circle(outputImage_points, cv::Point(point.x, point.y), 1, cv::Scalar(255,255,255), -1);
  // for (const auto& point : contour_right) 
  //       cv::circle(outputImage_points, cv::Point(point.x, point.y), 1, cv::Scalar(255,255,255), -1);

  //cv::drawContours(outputImage_points, std::vector<std::vector<cv::Point>>{contour_right}, 0, cv::Scalar(0, 0, 255), 1);

  /////////////////////////////////////////////////// Inicio  de Filtrado de la imagen depth //////////////////////////////////////////
 
  cv::Mat depthImage = cv_depthCam->image;
  cv::Mat depthValues;
  depthImage.convertTo(depthValues, CV_16U);


  // llamar a la función y guardar los resultados en dos objetos std::vector<cv::Point>
  auto[puntos_1, puntos_2] =processDepth(depthValues);

  thread_1.join();


  /////////////////////////////////////////////////////// resultados de los filtrados
  
  contour_left.insert(contour_left.end(), puntos_1.begin(), puntos_1.end());
  contour_right.insert(contour_right.end(), puntos_2.begin(), puntos_2.end());

  for (const auto& point : contour_left) 
        cv::circle(Image_lines, cv::Point(point.x, point.y), 1, cv::Scalar(0,0,0), -1);
  for (const auto& point : contour_right) 
        cv::circle(Image_lines, cv::Point(point.x, point.y), 1, cv::Scalar(0,0,0), -1);

   std::vector<std::vector<cv::Point>> contours_2;
   contours_2.push_back(contour_left);  
   contours_2.push_back(contour_right);

   contour_right.clear();
   contour_left.clear();



  /////////////////////////////////////////////////// Fin de flitrado de imagen depth /////////////////////////////////////////////////
  ///////////////////////////////////////////////////// Inicio del filtro de kalman/////////////////////////////
  /*Al ya tener todos los puntos de los bordes del panel tanto de los canales rgb como del canal depth realziamos un filtro de kalman
  de la linea que se ajusta a los dos conjuntos de puntos detectados.*/ 

  // Variables line current left and rigth para la estimacion de estados con filtro de kalman
  cv::Vec4f lc_l(0.0f, 0.0f, 0.0f, 0.0f);
  cv::Vec4f lc_r(0.0f, 0.0f, 0.0f, 0.0f);


  std::vector<cv::Vec4f> linesap;
  int cont_line = 0; // variable para contar si es la linea derecha o izqueirda

  for (const auto& contour : contours_2) {

      // Ajustar una línea a los contornos con fitline
      if (contour.size()==0)
        break; // si no existen contornos se rompe el for
      cv::Vec4f line;
      cv::fitLine(contour, line, cv::DIST_L1, 0, 0.001, 0.01);

      // caracteristicas de la linea vector en x, vector en y y punto inicial (x,y)
      float vx = line[0];
      float vy = line[1];
      float x  = line[2];
      float y  = line[3];

      if (inicio_kalman ){ // condicion para inicializar las varialbes del filtro de kalman
        
        // inicializar matriz P del filtro de kalman (para la linea derecha e izquierda)
        P_left << 1, 0, 0, 0
                 ,0, 1, 0, 0
                 ,0, 0, 1, 0
                 ,0, 0, 0, 1; 

        P_right = P_left;

        if (cont_line == 0){
          lc_l = line;
          curr_l << lc_l[0], lc_l[1], lc_l[2], lc_l[3]; // estados iniciales de la linea izqueirda
          x_estimate_l   = curr_l;
        }
        else{
          lc_r = line;        
          curr_r << lc_r[0], lc_r[1], lc_l[2], lc_l[3]; // estados iniciales de la linea derecha
          x_estimate_r   = curr_r;
        }              
        
       }

      else{   // Despues del inicializado del filtro de kalman

        if (cont_line == 0){
          lc_l = line;   
          curr_l << lc_l[0], lc_l[1], lc_l[2], lc_l[3];
        }
        else{
          lc_r = line;  
          curr_r << lc_r[0], lc_r[1], lc_r[2], lc_r[3];
        }
      }       

      // matrices para tener los resultados del filtro de kalman en la iteracion K para luego ser utilizada en k+1
      Eigen::VectorXd line_kalman_left(4);
      Eigen::VectorXd line_kalman_right(4);
      Eigen::MatrixXd outputP_left(4,4);
      Eigen::MatrixXd outputP_right(4,4);
      
      if (kalman_bool){
        if (cont_line == 0){
          std::tie(line_kalman_left, outputP_left)   = kalman_filter(curr_l,x_estimate_l, P_left, vel_body);
          P_left  = outputP_left;
          x_estimate_l = line_kalman_left;

        //Puntos de la linea izquierda filtrada 
          cv::Point pt1_kalma_left(x_estimate_l[2] - 1000 * x_estimate_l[0], x_estimate_l[3] - 1000 * x_estimate_l[1]);
          cv::Point pt2_kalma_left(x_estimate_l[2] + 1000 * x_estimate_l[0], x_estimate_l[3] + 1000 * x_estimate_l[1]); 
          float angle = std::atan2(pt1_kalma_left.y - pt2_kalma_left.y, pt1_kalma_left.x - pt2_kalma_left.x) * 180 / CV_PI;
          if (std::abs(angle) < 135 && std::abs(angle) > 45 )           
            cv::line(mono_resultImage, pt1_kalma_left, pt2_kalma_left, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
          cv::line(Image_lines, pt1_kalma_left, pt2_kalma_left, cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
        }
        else{
          std::tie(line_kalman_right, outputP_right) = kalman_filter(curr_r, x_estimate_r, P_right, vel_body);
          P_right = outputP_right;
          x_estimate_r = line_kalman_right;
          //Puntos de la linea izquierda filtrada 
          cv::Point pt1_kalma_right(x_estimate_r[2] - 1000 * x_estimate_r[0], x_estimate_r[3] - 1000 * x_estimate_r[1]);
          cv::Point pt2_kalma_right(x_estimate_r[2] + 1000 * x_estimate_r[0], x_estimate_r[3] + 1000 * x_estimate_r[1]); 
          float angle = std::atan2(pt1_kalma_right.y - pt2_kalma_right.y, pt1_kalma_right.x - pt2_kalma_right.x) * 180 / CV_PI;
          if (std::abs(angle) < 135 && std::abs(angle) > 45 ) 
            cv::line(mono_resultImage, pt1_kalma_right, pt2_kalma_right, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
          cv::line(Image_lines, pt1_kalma_right, pt2_kalma_right, cv::Scalar(255, 0, 0), 2, cv::LINE_AA);
        }
      }
     

      if(kalman_bool == false){
        //Puntos de la linea sin filtrar
        cv::Point pt1_out_lin(x - 1000 * vx, y - 1000 * vy);
        cv::Point pt2_out_lin(x + 1000 * vx, y + 1000 * vy); 
        float angle = std::atan2(pt1_out_lin.y - pt2_out_lin.y, pt1_out_lin.x - pt2_out_lin.x) * 180 / CV_PI;
        if (std::abs(angle) < 135 && std::abs(angle) > 45 ) 
          cv::line(mono_resultImage, pt1_out_lin, pt2_out_lin, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        cv::line(Image_lines, pt1_out_lin, pt2_out_lin, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
      }
      cont_line ++;
      
   }

  inicio_kalman = false; // variable que rompe la inicializado del filtro de kalman

  ///////////////////////////////////////////////////////// fin filtro kalman      

  
  ros::Time end_time = ros::Time::now();  
  // Calcular la diferencia de tiempo
  ros::Duration delay_ros = end_time - start_time;
  
  sensor_msgs::ImagePtr image_msg_mask;
  image_msg_mask = cv_bridge::CvImage(std_msgs::Header(), "mono8", mono_resultImage).toImageMsg();
  image_msg_mask->header = in_rgb->header;
  image_msg_mask->header.stamp = in_rgb->header.stamp + delay_ros;
  pub_img_out.publish(image_msg_mask);  

  sensor_msgs::ImagePtr image_msg_rgb;
  image_msg_rgb = cv_bridge::CvImage(std_msgs::Header(), "bgr8", Image_lines).toImageMsg();
  image_msg_rgb->header = in_rgb->header;
  image_msg_rgb->header.stamp = in_rgb->header.stamp + delay_ros;
  panel_LinesFeatures_pub.publish(image_msg_rgb);  

  // sensor_msgs::ImagePtr image_msg_hsv;
  // image_msg_hsv = cv_bridge::CvImage(std_msgs::Header(), "mono8", gray_image_filter).toImageMsg();
  // image_msg_hsv->header = in_rgb->header;
  // image_msg_hsv->header.stamp = in_rgb->header.stamp + delay_ros;
  // panel_hsvfilter.publish(image_msg_hsv);  

  auto t11= Clock::now();
  std::cout<<"time total (ms): "<<std::chrono::duration_cast<std::chrono::nanoseconds>(t11-t1).count()/1000000.0<<std::endl;
    // mensaje de tiempo de ejecucion
  // vision_matrice100::DurationStamped time_msg;  // Crear una instancia del mensaje
  // time_msg.header.stamp = in_rgb->header.stamp + delay_ros;  // Asignar la marca de tiempo actual al encabezado
  // time_msg.data = delay_ros;  // Asignar el valor a publicar al campo 'data' del mensaje
  // time_pub.publish(time_msg);  // Publicar el mensaje

}

int main(int argc, char** argv)
{

  ros::init(argc, argv, "panelFeatures_kalman");
  ros::NodeHandle nh;  

  // parametros para imagen a color
  nh.getParam("/area_filter", area_filter); 
  nh.getParam("/real_sim",real_sim);
  nh.getParam("/hsv_v", hsv_v); 
  nh.getParam("/hsv_s",hsv_s);
 
  // topics de la imagen 
  nh.getParam("/rgb_Topic", rgb_Topic);
  nh.getParam("/depth_Topic", depth_Topic);
  nh.getParam("/odom_Topic", odom_Topic);

  // message_filters::Subscriber<Image>  rgb_sub(nh, rgb_Topic , 10);
  // message_filters::Subscriber<Image>  depth_sub(nh, depth_Topic, 10);
  // typedef sync_policies::ApproximateTime<Image, Image> MySyncPolicy;
  // Synchronizer<MySyncPolicy> sync(MySyncPolicy(50), rgb_sub, depth_sub);
  // sync.registerCallback(boost::bind(&callback, _1, _2 ));

  message_filters::Subscriber<Image>  rgb_sub(nh, rgb_Topic , 10);
  message_filters::Subscriber<Image>  depth_sub(nh, depth_Topic, 10);
  message_filters::Subscriber<nav_msgs::Odometry>  odom_sub(nh, odom_Topic, 10);
  typedef sync_policies::ApproximateTime<Image, Image, nav_msgs::Odometry> MySyncPolicy;
  Synchronizer<MySyncPolicy> sync(MySyncPolicy(100), rgb_sub, depth_sub, odom_sub);
  sync.registerCallback(boost::bind(&callback, _1, _2 , _3));
  
  pub_img_out = nh.advertise<sensor_msgs::Image>("/panel/image/mask", 10);
  panel_LinesFeatures_pub = nh.advertise<sensor_msgs::Image>("/panel/image/rgb_mask", 10);
  // time_pub = nh.advertise<vision_matrice100::DurationStamped>("/panel/image/runtime", 10);  


  ros::spin();
}

